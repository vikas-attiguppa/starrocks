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


package com.starrocks.sql.optimizer.rule.implementation;

import com.starrocks.analysis.JoinOperator;
import com.starrocks.sql.optimizer.JoinHelper;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.Utils;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalJoinOperator;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.operator.scalar.BinaryPredicateOperator;
import com.starrocks.sql.optimizer.rule.RuleType;

import java.util.List;

public abstract class JoinImplementationRule extends ImplementationRule {

    protected List<BinaryPredicateOperator> extractEqPredicate(OptExpression input, OptimizerContext context) {
        ColumnRefSet leftChildColumns = input.inputAt(0).getLogicalProperty().getOutputColumns();
        ColumnRefSet rightChildColumns = input.inputAt(1).getLogicalProperty().getOutputColumns();
        LogicalJoinOperator joinOperator = (LogicalJoinOperator) input.getOp();
        return JoinHelper.getEqualsPredicate(leftChildColumns, rightChildColumns,
                Utils.extractConjuncts(joinOperator.getOnPredicate()));
    }

    protected JoinOperator getJoinType(OptExpression input) {
        LogicalJoinOperator joinOperator = (LogicalJoinOperator) input.getOp();
        return joinOperator.getJoinType();
    }

    protected JoinImplementationRule(RuleType type) {
        super(type, Pattern.create(OperatorType.LOGICAL_JOIN).
                addChildren(Pattern.create(OperatorType.PATTERN_LEAF), Pattern.create(OperatorType.PATTERN_LEAF)));
    }

}
