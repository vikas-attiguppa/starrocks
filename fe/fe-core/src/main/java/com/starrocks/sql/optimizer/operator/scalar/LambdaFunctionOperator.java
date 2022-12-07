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


package com.starrocks.sql.optimizer.operator.scalar;

import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;
import com.starrocks.catalog.Type;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.OperatorType;

import java.util.List;
import java.util.Objects;

public class LambdaFunctionOperator extends ScalarOperator {

    private List<ColumnRefOperator> refColumns;
    private ScalarOperator lambdaExpr;

    public LambdaFunctionOperator(List<ColumnRefOperator> refColumns, ScalarOperator lambdaExpr, Type retType) {
        super(OperatorType.LAMBDA_FUNCTION, retType);
        this.refColumns = refColumns;
        this.lambdaExpr = lambdaExpr;
    }

    public List<ColumnRefOperator> getRefColumns() {
        return refColumns;
    }

    public ScalarOperator getLambdaExpr() {
        return lambdaExpr;
    }

    // only need to concern the lambda expression.
    @Override
    public List<ScalarOperator> getChildren() {
        return Lists.newArrayList(lambdaExpr);
    }

    @Override
    public ScalarOperator getChild(int index) {
        Preconditions.checkState(index == 0);
        return lambdaExpr;
    }

    @Override
    public boolean isNullable() {
        return lambdaExpr.isNullable();
    }

    @Override
    public void setChild(int index, ScalarOperator child) {
        Preconditions.checkState(index == 0);
        this.lambdaExpr = child;
    }

    @Override
    public String toString() {
        return "(" + refColumns.toString() + "->" + lambdaExpr.toString() + ")";
    }

    @Override
    public int hashCode() {
        return Objects.hash(getType(), refColumns, lambdaExpr);
    }

    @Override
    public boolean equals(Object other) {
        if (other == null) {
            return false;
        }
        if (this == other) {
            return true;
        }
        if (other instanceof LambdaFunctionOperator) {
            final LambdaFunctionOperator lambda = (LambdaFunctionOperator) other;
            return lambda.getType().equals(getType()) && lambda.lambdaExpr.equals(lambdaExpr) &&
                    lambda.refColumns.equals(refColumns);
        }
        return false;
    }

    @Override
    public <R, C> R accept(ScalarOperatorVisitor<R, C> visitor, C context) {
        return visitor.visitLambdaFunctionOperator(this, context);
    }

    @Override
    public ColumnRefSet getUsedColumns() {
        return lambdaExpr.getUsedColumns();
    }

    @Override
    public ScalarOperator clone() {
        LambdaFunctionOperator clone = (LambdaFunctionOperator) super.clone();
        List<ColumnRefOperator> refs = Lists.newArrayList();
        this.refColumns.forEach(p -> refs.add((ColumnRefOperator) p.clone()));
        clone.refColumns = refs;
        clone.lambdaExpr = this.lambdaExpr.clone();
        return clone;
    }
}
