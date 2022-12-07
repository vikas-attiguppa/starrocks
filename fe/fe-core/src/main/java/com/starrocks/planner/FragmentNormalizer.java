// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


package com.starrocks.planner;

import com.clearspring.analytics.util.Lists;
import com.google.common.base.Preconditions;
import com.google.common.collect.Maps;
import com.google.common.collect.Range;
import com.google.common.collect.Sets;
import com.starrocks.analysis.BetweenPredicate;
import com.starrocks.analysis.BinaryPredicate;
import com.starrocks.analysis.CompoundPredicate;
import com.starrocks.analysis.Expr;
import com.starrocks.analysis.FunctionCallExpr;
import com.starrocks.analysis.InPredicate;
import com.starrocks.analysis.LiteralExpr;
import com.starrocks.analysis.NullLiteral;
import com.starrocks.analysis.SlotId;
import com.starrocks.analysis.SlotRef;
import com.starrocks.analysis.TupleId;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.catalog.KeysType;
import com.starrocks.catalog.PartitionKey;
import com.starrocks.catalog.RangePartitionInfo;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.IdGenerator;
import com.starrocks.common.Pair;
import com.starrocks.sql.plan.ExecPlan;
import com.starrocks.thrift.TCacheParam;
import com.starrocks.thrift.TNormalPlanNode;
import com.starrocks.thrift.TExpr;
import org.apache.thrift.TException;
import org.apache.thrift.TSerializer;
import org.apache.thrift.protocol.TCompactProtocol;

import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.stream.Collectors;

// FragmentNormalizer is used to normalize a cacheable Fragment. After a cacheable Fragment
// is normalized, FragmentNormalizer draws out required information as follows from the fragment.
// 1. MD5 digest: semantically-equivalent Fragments always produce the same MD5 digest.
// 2. cache interpolation point: it is a PlanNodeId designated a PlanNode above which the CacheOperator
//    shall be interpolated, the CacheOperator is used to populate/probe the per-tablet result of this
//    PlanNode in the query cache.
// 3. output SlotId remapping: the output result of cache interpolation point of semantically-equivalent
//    Fragments may have different real SlotIds and the same remapped SlotIds. so before the result is
//    populated into the cache, we must translate the real SlotIds in result to remapped SlotIds; after the
//    result is probed and read out from the cache, we must translate the remapped SlotIds to real SlotIds.
// 4. RangeMap: it records mapping from partition id to decomposed region of the simple range partition predicate.
public class FragmentNormalizer {
    private ExecPlan execPlan;
    private PlanFragment fragment;
    private Map<PlanNodeId, PlanNodeId> planNodeIdRemapping = Maps.newHashMap();
    private Map<SlotId, SlotId> slotIdRemapping = Maps.newHashMap();
    private Map<TupleId, TupleId> tupleIdRemapping = Maps.newHashMap();
    private IdGenerator<PlanNodeId> planNodeIdGen = PlanNodeId.createGenerator();
    private IdGenerator<TupleId> tupleIdIdGen = TupleId.createGenerator();
    private IdGenerator<SlotId> slotIdGen = SlotId.createGenerator();
    private List<TNormalPlanNode> normalizedPlanNodes = Lists.newArrayList();
    private Map<Long, String> selectedRangeMap = Maps.newHashMap();
    private boolean uncacheable = false;
    private boolean canUseMultiVersion = true;

    private KeysType keysType;

    private Set<SlotId> slotsUseAggColumns;

    private boolean processingLeftNode = false;

    public FragmentNormalizer(ExecPlan execPlan, PlanFragment fragment) {
        this.execPlan = execPlan;
        this.fragment = fragment;
    }

    static Range<PartitionKey> toClosedOpenRange(Range<PartitionKey> range) {
        PartitionKey lowerBound = range.lowerEndpoint();
        PartitionKey upperBound = range.upperEndpoint();
        if (!lowerBound.isMinValue() && !range.contains(lowerBound)) {
            lowerBound = lowerBound.successor();
        }

        if (!upperBound.isMaxValue() && range.contains(upperBound)) {
            upperBound = upperBound.successor();
        }
        return Range.closedOpen(lowerBound, upperBound);
    }

    public boolean isUncacheable() {
        return uncacheable;
    }

    public void setUncacheable(boolean uncacheable) {
        this.uncacheable = uncacheable;
    }

    public void setCanUseMultiVersion(boolean canUse) {
        canUseMultiVersion = canUse;
    }

    void beginProcessingLeftNode(boolean v) {
        this.processingLeftNode = v;
    }

    void endProcessingLeftNode() {
        this.processingLeftNode = false;
    }

    boolean isProcessingLeftNode() {
        return this.processingLeftNode;
    }

    public boolean isCanUseMultiVersion() {
        return canUseMultiVersion;
    }

    public void setKeysType(KeysType keysType) {
        this.keysType = keysType;
    }

    public KeysType getKeysType() {
        return keysType;
    }

    private static String toHexString(byte[] bytes) {
        StringBuffer s = new StringBuffer(bytes.length * 2);
        char[] d = "0123456789abcdef".toCharArray();
        for (byte a : bytes) {
            s.append(d[(a >>> 4) & 0xf]);
            s.append(d[a & 0xf]);
        }
        return s.toString();
    }

    public ExecPlan getExecPlan() {
        return execPlan;
    }

    public List<Integer> remapTupleIds(List<TupleId> ids) {
        return ids.stream().map(id -> remapTupleId(id).asInt()).collect(Collectors.toList());
    }

    public PlanNodeId remapPlanNodeId(PlanNodeId planNodeId) {
        return planNodeIdRemapping.computeIfAbsent(planNodeId, arg -> planNodeIdGen.getNextId());
    }

    public SlotId remapSlotId(SlotId slotId) {
        return slotIdRemapping.computeIfAbsent(slotId, arg -> slotIdGen.getNextId());
    }

    public Integer remapSlotId(Integer slotId) {
        return slotIdRemapping.computeIfAbsent(new SlotId(slotId), arg -> slotIdGen.getMaxId()).asInt();
    }

    public List<Integer> remapSlotIds(List<SlotId> slotIds) {
        return slotIds.stream().map(this::remapSlotId).map(SlotId::asInt).collect(Collectors.toList());
    }

    public List<Integer> remapIntegerSlotIds(List<Integer> slotIds) {
        return slotIds.stream().map(this::remapSlotId).collect(Collectors.toList());
    }

    public boolean containsAllSlotIds(List<SlotId> slotIds) {
        return slotIds.stream().allMatch(slotIdRemapping::containsKey);
    }

    public TupleId remapTupleId(TupleId tupleId) {
        return tupleIdRemapping.computeIfAbsent(tupleId, arg -> tupleIdIdGen.getNextId());
    }

    public ByteBuffer normalizeExpr(Expr expr) {
        uncacheable = uncacheable || hasNonDeterministicFunctions(expr);
        TExpr texpr = expr.normalize(this);
        TSerializer ser = new TSerializer(new TCompactProtocol.Factory());
        try {
            return ByteBuffer.wrap(ser.serialize(texpr));
        } catch (Exception ignored) {
            Preconditions.checkArgument(false);
        }
        return null;
    }

    public Pair<List<Integer>, List<ByteBuffer>> normalizeSlotIdsAndExprs(Map<SlotId, Expr> exprMap) {
        List<Pair<SlotId, ByteBuffer>> slotIdsAndStringFunctions = exprMap.entrySet().stream()
                .map(e -> new Pair<>(e.getKey(), normalizeExpr(e.getValue())))
                .sorted(Pair.comparingBySecond()).collect(Collectors.toList());
        List<SlotId> slotIds = slotIdsAndStringFunctions.stream().map(e -> e.first).collect(Collectors.toList());
        List<ByteBuffer> exprs = slotIdsAndStringFunctions.stream().map(e -> e.second).collect(Collectors.toList());
        return new Pair<>(remapSlotIds(slotIds), exprs);
    }

    public List<ByteBuffer> normalizeExprs(List<Expr> exprList) {
        if (exprList == null || exprList.isEmpty()) {
            return Collections.emptyList();
        }
        return exprList.stream().map(this::normalizeExpr).sorted(ByteBuffer::compareTo).collect(Collectors.toList());
    }

    public List<ByteBuffer> normalizeOrderedExprs(List<Expr> exprList) {
        if (exprList == null || exprList.isEmpty()) {
            return Collections.emptyList();
        }
        return exprList.stream().map(this::normalizeExpr).collect(Collectors.toList());
    }

    public boolean computeDigest(PlanNode cachePointNode) {
        try {
            if (uncacheable || selectedRangeMap.isEmpty()) {
                return false;
            }
            TSerializer serializer = new TSerializer(new TCompactProtocol.Factory());
            MessageDigest digest = MessageDigest.getInstance("SHA-256");

            for (TNormalPlanNode node : normalizedPlanNodes) {
                byte[] data = serializer.serialize(node);
                digest.update(data);
            }
            List<SlotId> slotIds = cachePointNode.getOutputSlotIds(execPlan.getDescTbl());
            List<Integer> remappedSlotIds = remapSlotIds(slotIds);
            Map<Integer, Integer> outputSlotIdRemapping = Maps.newHashMap();
            for (int i = 0; i < slotIds.size(); ++i) {
                outputSlotIdRemapping.put(slotIds.get(i).asInt(), remappedSlotIds.get(i));
            }
            TCacheParam cacheParam = new TCacheParam();
            cacheParam.setId(cachePointNode.getId().asInt());
            cacheParam.setDigest(ByteBuffer.wrap(digest.digest()));
            cacheParam.setSlot_remapping(outputSlotIdRemapping);
            cacheParam.setRegion_map(selectedRangeMap);
            cacheParam.setCan_use_multiversion(canUseMultiVersion);
            cacheParam.setKeys_type(keysType.toThrift());
            fragment.setCacheParam(cacheParam);
            return true;
        } catch (TException | NoSuchAlgorithmException e) {
            throw new RuntimeException("Fatal error happens when normalize PlanFragment", e);
        }
    }

    public void normalizeSubTree(Set<PlanNodeId> leftNodeIds, PlanNode node) {
        for (PlanNode child : node.getChildren()) {
            if (uncacheable) {
                return;
            }
            normalizeSubTree(leftNodeIds, child);
        }

        if (uncacheable) {
            return;
        }

        beginProcessingLeftNode(leftNodeIds.contains(node.getId()));
        if (isProcessingLeftNode() && (node instanceof ProjectNode) && ((ProjectNode) node).isTrivial()) {
            return;
        }
        TNormalPlanNode canonNode = node.normalize(this);
        normalizedPlanNodes.add(canonNode);
        endProcessingLeftNode();
    }

    List<Expr> flatAndPredicate(Expr conjunct) {
        if (!(conjunct instanceof CompoundPredicate)) {
            return Arrays.asList(conjunct);
        }
        CompoundPredicate compoundPredicate = (CompoundPredicate) conjunct;
        if (compoundPredicate.getOp() != CompoundPredicate.Operator.AND) {
            return Arrays.asList(conjunct);
        } else {
            return compoundPredicate.getChildren().stream()
                    .flatMap(child -> flatAndPredicate(child).stream()).collect(Collectors.toList());
        }
    }

    boolean isSimpleRegionPredicate(Expr expr) {

        if (!(expr instanceof BetweenPredicate) && !(expr instanceof BinaryPredicate)) {
            return false;
        }
        boolean simple = expr.getChild(0) instanceof SlotRef &&
                expr.getChildren().subList(1, expr.getChildren().size())
                        .stream().allMatch(e -> (e instanceof LiteralExpr) && !(e instanceof NullLiteral));
        if (!simple) {
            return false;
        }
        if (expr instanceof BetweenPredicate) {
            return !((BetweenPredicate) expr).isNotBetween();
        }
        if (expr instanceof BinaryPredicate) {
            return ((BinaryPredicate) expr).getOp() != BinaryPredicate.Operator.EQ_FOR_NULL;
        }
        return true;
    }

    boolean hasNonDeterministicFunctions(Expr expr) {
        if (expr instanceof FunctionCallExpr) {
            FunctionCallExpr callExpr = (FunctionCallExpr) expr;
            if (FunctionSet.nonDeterministicFunctions.contains(callExpr.getFn().functionName())) {
                return true;
            }
        }
        return expr.getChildren().stream().anyMatch(e -> hasNonDeterministicFunctions(e));
    }

    List<Range<PartitionKey>> convertPredicateToRange(Column partitionColumn, Expr expr) {
        List<Range<PartitionKey>> result = Lists.newArrayList();
        PartitionKey minKey = null;
        PartitionKey maxKey = null;
        try {
            minKey = PartitionKey.createInfinityPartitionKey(Arrays.asList(partitionColumn), false);
            maxKey = PartitionKey.createInfinityPartitionKey(Arrays.asList(partitionColumn), true);
        } catch (AnalysisException ignored) {
        }
        Preconditions.checkArgument(minKey != null && maxKey != null);
        if (expr instanceof BinaryPredicate) {
            BinaryPredicate predicate = (BinaryPredicate) expr;
            if (predicate.getOp() == BinaryPredicate.Operator.EQ_FOR_NULL) {
                return result;
            }
            LiteralExpr rhs = (LiteralExpr) predicate.getChild(1);
            PartitionKey rhsKey = new PartitionKey();
            rhsKey.pushColumn(rhs, partitionColumn.getPrimitiveType());
            switch (predicate.getOp()) {
                case EQ:
                    result.add(Range.closed(rhsKey, rhsKey));
                    break;
                case NE:
                    result.add(Range.open(minKey, rhsKey));
                    result.add(Range.open(rhsKey, maxKey));
                    break;
                case LE:
                    result.add(Range.openClosed(minKey, rhsKey));
                    break;
                case GE:
                    result.add(Range.closedOpen(rhsKey, maxKey));
                    break;
                case LT:
                    result.add(Range.open(minKey, rhsKey));
                    break;
                case GT:
                    result.add(Range.open(rhsKey, maxKey));
                    break;
                case EQ_FOR_NULL:
                    break;
            }
            return result;
        } else if (expr instanceof BetweenPredicate) {
            BetweenPredicate predicate = (BetweenPredicate) expr;
            LiteralExpr lowerBound = (LiteralExpr) expr.getChild(1);
            LiteralExpr upperBound = (LiteralExpr) expr.getChild(2);
            PartitionKey lowerKey = new PartitionKey();
            PartitionKey upperKey = new PartitionKey();
            lowerKey.pushColumn(lowerBound, partitionColumn.getPrimitiveType());
            lowerKey.pushColumn(upperBound, partitionColumn.getPrimitiveType());
            if (predicate.isNotBetween()) {
                result.add(Range.open(minKey, lowerKey));
                result.add(Range.open(upperKey, upperKey));
            } else {
                result.add(Range.closed(lowerKey, upperKey));
            }
            return result;
        } else if (expr instanceof InPredicate) {
            InPredicate predicate = (InPredicate) expr;
            for (Expr elem : predicate.getListChildren()) {
                LiteralExpr literal = (LiteralExpr) elem;
                PartitionKey key = new PartitionKey();
                key.pushColumn(literal, partitionColumn.getPrimitiveType());
                if (predicate.isNotIn()) {
                    result.add(Range.open(minKey, key));
                    result.add(Range.open(key, maxKey));
                } else {
                    result.add(Range.closed(key, key));
                }
            }
            return result;
        } else {
            return Lists.newArrayList();
        }
    }

    List<Expr> getPartitionRangePredicates(List<Expr> conjuncts,
                                           List<Map.Entry<Long, Range<PartitionKey>>> rangeMap,
                                           RangePartitionInfo partitionInfo,
                                           SlotId partitionSlotId) {

        List<Expr> exprs = conjuncts.stream().flatMap(e -> flatAndPredicate(e).stream()).collect(Collectors.toList());
        List<Expr> unboundExprs = Lists.newArrayList();
        List<Expr> boundSimpleRegionExprs = Lists.newArrayList();
        List<Expr> boundOtherExprs = Lists.newArrayList();
        for (Expr e : exprs) {
            if (!e.isBound(partitionSlotId)) {
                unboundExprs.add(e);
                continue;
            }
            if (isSimpleRegionPredicate(e)) {
                boundSimpleRegionExprs.add(e);
            } else {
                boundOtherExprs.add(e);
            }
        }

        // TODO(by satanson): If the bound exprs contain no simple range exprs but only contain complex exprs, we
        //  create a simpleRangeMap without predicates' decomposition to turn on the cache. date_trunc function
        //  is frequently-used, we should decompose predicates contains date_trunc in the future.
        if (!boundOtherExprs.isEmpty() && boundSimpleRegionExprs.isEmpty()) {
            createSimpleRangeMap(rangeMap.stream().map(Map.Entry::getKey).collect(Collectors.toSet()));
            return conjuncts;
        }

        if (boundSimpleRegionExprs.isEmpty()) {
            for (Map.Entry<Long, Range<PartitionKey>> range : rangeMap) {
                selectedRangeMap.put(range.getKey(), range.getValue().toString());
            }
            return conjuncts;
        }

        Column partitionColumn = partitionInfo.getPartitionColumns().get(0);
        List<Range<PartitionKey>> partitionRanges = rangeMap.stream()
                .map(Map.Entry::getValue).collect(Collectors.toList());

        // compute the intersection region of partition range and region predicates
        for (Expr expr : boundSimpleRegionExprs) {
            List<Range<PartitionKey>> ranges = convertPredicateToRange(partitionColumn, expr);
            if (ranges.isEmpty()) {
                continue;
            }
            for (Range<PartitionKey> r : ranges) {
                partitionRanges = partitionRanges.stream().filter(pr ->
                        pr.isConnected(r)).map(pr -> pr.intersection(r)).collect(Collectors.toList());
            }
        }
        // select the partition ranges should be cached
        for (int i = 0; i < partitionRanges.size(); ++i) {
            Range<PartitionKey> range = partitionRanges.get(i);
            if (range.isEmpty()) {
                continue;
            }
            range = toClosedOpenRange(range);
            Map.Entry<Long, Range<PartitionKey>> partitionKeyRange = rangeMap.get(i);
            // when the range is to total cover this partition, we also cache it
            if (!range.isEmpty()) {
                selectedRangeMap.put(partitionKeyRange.getKey(), range.toString());
            }
        }
        // After we decompose the predicates, we should create a simple selectedRangeMap to turn on query cache if
        // we get a empty selectedRangeMap. it is defensive-style programming.
        if (selectedRangeMap.isEmpty()) {
            createSimpleRangeMap(rangeMap.stream().map(Map.Entry::getKey).collect(Collectors.toSet()));
            return conjuncts;
        } else {
            List<Expr> remainConjuncts = Lists.newArrayList();
            remainConjuncts.addAll(unboundExprs);
            remainConjuncts.addAll(boundOtherExprs);
            return remainConjuncts;
        }
    }

    // For partition that not support partition column range predicates' decomposition, we
    // just create a simple selectedRangeMap which is used to construct cache key in BE.
    public void createSimpleRangeMap(Collection<Long> selectedPartitionIds) {
        selectedRangeMap = Maps.newHashMap();
        selectedPartitionIds.stream().forEach(id -> selectedRangeMap.put(id, "[]"));
    }

    public Set<SlotId> getSlotsUseAggColumns() {
        return slotsUseAggColumns;
    }

    public void setSlotsUseAggColumns(Set<SlotId> slotsUseAggColumns) {
        this.slotsUseAggColumns = slotsUseAggColumns;
    }

    public void addSlotsUseAggColumns(Map<SlotId, Expr> exprs) {
        exprs.forEach((slotId, expr) -> {
            List<SlotRef> slotRefs = Lists.newArrayList();
            expr.collect(SlotRef.class, slotRefs);
            Set<SlotId> usedColumnIds = slotRefs.stream().map(SlotRef::getSlotId).collect(Collectors.toSet());
            if (!Sets.intersection(this.slotsUseAggColumns, usedColumnIds).isEmpty()) {
                this.slotsUseAggColumns.add(slotId);
            }
        });
    }

    public void disableMultiversionIfExprsUseAggColumns(List<Expr> exprs) {
        if (exprs == null || exprs.isEmpty()) {
            return;
        }
        List<SlotRef> slotRefs = Lists.newArrayList();
        exprs.forEach(e -> e.collect(SlotRef.class, slotRefs));
        Set<SlotId> usedColumnIds = slotRefs.stream().map(SlotRef::getSlotId).collect(Collectors.toSet());
        if (!Sets.intersection(usedColumnIds, this.slotsUseAggColumns).isEmpty()) {
            this.setCanUseMultiVersion(false);
        }
    }

    public static boolean isAllowedInLeftMostPath(PlanNode node) {
        if (node instanceof AggregationNode) {
            return true;
        } else if (node instanceof DecodeNode) {
            return true;
        } else if (node instanceof ProjectNode) {
            return true;
        } else if (node instanceof SelectNode) {
            return true;
        } else if (node instanceof TableFunctionNode) {
            return true;
        } else if (node instanceof RepeatNode) {
            return true;
        } else if (node instanceof HashJoinNode) {
            return true;
        } else if (node instanceof NestLoopJoinNode) {
            return true;
        } else if (node instanceof OlapScanNode) {
            return true;
        } else {
            return false;
        }
    }

    public static void collectRightSiblingFragments(PlanNode root, List<PlanFragment> siblings) {
        if (root.getChildren().isEmpty()) {
            return;
        }

        if (root instanceof ExchangeNode) {
            root.getChildren().forEach(child -> siblings.add(child.getFragment()));
        }
        root.getChildren().forEach(child -> collectRightSiblingFragments(child, siblings));
    }

    public static boolean isTransformJoin(PlanNode joinNode) {
        if (joinNode instanceof NestLoopJoinNode) {
            return true;
        } else if (joinNode instanceof HashJoinNode) {
            HashJoinNode hashJoinNode = (HashJoinNode) joinNode;
            return hashJoinNode.getJoinOp().isLeftTransform() && !hashJoinNode.getDistrMode().areBothSidesShuffled();
        } else {
            return false;
        }
    }

    public boolean normalize() {
        PlanNode root = fragment.getPlanRoot();

        // Get leftmost path
        List<PlanNode> leftNodes = Lists.newArrayList();
        for (PlanNode currNode = root; currNode != null && currNode.getFragment() == fragment;
                currNode = currNode.getChild(0)) {
            leftNodes.add(currNode);
        }

        Preconditions.checkState(!leftNodes.isEmpty());
        // Not cacheable unless the leftmost PlanNode is OlapScanNode
        if (!(leftNodes.get(leftNodes.size() - 1) instanceof OlapScanNode)) {
            return false;
        }

        AggregationNode firstAggNode = null;
        List<JoinNode> joinNodes = Lists.newArrayList();
        PlanNode topMostDigestNode = null;
        for (int i = leftNodes.size() - 1; i >= 0; --i) {
            PlanNode node = leftNodes.get(i);
            if (!isAllowedInLeftMostPath(node)) {
                break;
            }

            if (firstAggNode == null && (node instanceof AggregationNode)) {
                firstAggNode = (AggregationNode) node;
                continue;
            }

            if (node instanceof JoinNode) {
                JoinNode joinNode = (JoinNode) node;
                joinNodes.add(joinNode);
                // JoinNode below aggNode must be a transform one
                if (firstAggNode == null && !isTransformJoin(joinNode)) {
                    return false;
                }
                // JoinNode above aggNode that having runtime filters should be packed into digest.
                if (firstAggNode != null && !joinNode.getBuildRuntimeFilters().isEmpty()) {
                    topMostDigestNode = joinNode;
                }
            }
        }

        // Not cacheable unless Aggregation node is found
        if (firstAggNode == null) {
            return false;
        }

        // If there exists no JoinNode has runtime filters above cache point(i.e.firstAggNode),
        // then we just compute digest from the subtree rooted at firstAggNode.
        if (topMostDigestNode == null) {
            topMostDigestNode = firstAggNode;
        }

        // Not cacheable unless alien GRF(s) take effects on this PlanFragment.
        // The alien GRF(s) mean the GRF(S) that not created by PlanNodes of the subtree rooted at
        // the PlanFragment.planRoot.
        Set<Integer> grfBuilders =
                fragment.getProbeRuntimeFilters().values().stream().filter(RuntimeFilterDescription::isHasRemoteTargets)
                        .map(RuntimeFilterDescription::getBuildPlanNodeId).collect(Collectors.toSet());
        if (!grfBuilders.isEmpty()) {
            List<PlanFragment> rightSiblings = Lists.newArrayList();
            collectRightSiblingFragments(root, rightSiblings);
            Set<Integer> acceptableGrfBuilders = rightSiblings.stream().flatMap(
                    frag -> frag.getBuildRuntimeFilters().values().stream().map(
                            RuntimeFilterDescription::getBuildPlanNodeId)).collect(Collectors.toSet());
            boolean hasAlienGrf = !Sets.difference(grfBuilders, acceptableGrfBuilders).isEmpty();
            if (hasAlienGrf) {
                return false;
            }
        }
        Set<PlanNodeId> leftNodeIds = leftNodes.stream().map(PlanNode::getId).collect(Collectors.toSet());
        normalizeSubTree(leftNodeIds, topMostDigestNode);
        return computeDigest(firstAggNode);
    }
}
