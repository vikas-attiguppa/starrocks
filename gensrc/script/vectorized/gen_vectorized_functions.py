#!/usr/bin/env python
# encoding: utf-8

"""
  Copyright 2021-present StarRocks, Inc. All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

       https://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
"""

import os
import sys
from string import Template

import vectorized_functions

sys.path.append(os.path.abspath(os.path.dirname(os.path.dirname(__file__))))

license_string = """
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

// This is a generated file, DO NOT EDIT.
// To add new functions, see the generator at
// common/function-registry/gen_builtins_catalog.py or the function list at
// common/function-registry/starrocks_builtins_functions.py.
"""

java_template = Template("""
${license}

package com.starrocks.builtins;

import com.starrocks.catalog.FunctionSet;
import com.starrocks.catalog.Type;

public class VectorizedBuiltinFunctions {
    public static void initBuiltins(FunctionSet functionSet) {
        ${functions}
  }
}

""")

cpp_template = Template("""
${license}

#include "exprs/array_functions.h"
#include "exprs/builtin_functions.h"
#include "exprs/map_functions.h"
#include "exprs/math_functions.h"
#include "exprs/bit_functions.h"
#include "exprs/string_functions.h"
#include "exprs/time_functions.h"
#include "exprs/like_predicate.h"
#include "exprs/is_null_predicate.h"
#include "exprs/hyperloglog_functions.h"
#include "exprs/bitmap_functions.h"
#include "exprs/json_functions.h"
#include "exprs/hash_functions.h"
#include "exprs/encryption_functions.h"
#include "exprs/geo_functions.h"
#include "exprs/percentile_functions.h"
#include "exprs/grouping_sets_functions.h"
#include "exprs/es_functions.h"
#include "exprs/utility_functions.h"

namespace starrocks {

BuiltinFunctions::FunctionTables BuiltinFunctions::_fn_tables = {
        ${functions}
};

}
""")

function_list = list()

def add_function(fn_data):
    entry = dict()
    entry["id"] = fn_data[0]
    entry["name"] = fn_data[1]
    entry["ret"] = fn_data[2]
    entry["args"] = fn_data[3]

    if "..." in fn_data[3]:
        assert 2 <= len(fn_data[3]), "Invalid arguments in vectorized_functions.py:\n\t" + repr(fn_data)
        assert "..." == fn_data[3][-1], "variadic parameter must at the end:\n\t" + repr(fn_data)

        entry["args_nums"] = len(fn_data[3]) - 1
    else:
        entry["args_nums"] = len(fn_data[3])

    entry["fn"] = "&" + fn_data[4] if fn_data[4] != "nullptr" else "nullptr"

    if len(fn_data) >= 7:
        entry["prepare"] = "&" + fn_data[5] if fn_data[5] != "nullptr" else "nullptr"
        entry["close"] = "&" + fn_data[6] if fn_data[6] != "nullptr" else "nullptr"
    
    if fn_data[-1] == True or fn_data[-1] == False:
        entry["exception_safe"] = str(fn_data[-1]).lower()
    else:
        entry["exception_safe"] = "true"

    function_list.append(entry)


def generate_fe(path):
    fn_template = Template(
        'functionSet.addVectorizedScalarBuiltin(${id}, "${name}", ${has_vargs}, Type.${ret}${args_types});')

    def gen_fe_fn(fnm):
        fnm["args_types"] = ", " if len(fnm["args"]) > 0 else ""
        fnm["args_types"] = fnm["args_types"] + ", ".join(["Type." + i for i in fnm["args"] if i != "..."])
        fnm["has_vargs"] = "true" if "..." in fnm["args"] else "false"

        return fn_template.substitute(fnm)

    value = dict()
    value["license"] = license_string
    value["functions"] = "\n        ".join([gen_fe_fn(i) for i in function_list])

    content = java_template.substitute(value)

    with open(path, mode="w+") as f:
        f.write(content)


def generate_cpp(path):
    def gen_be_fn(fnm):
        res = ""
        if "prepare" in fnm:
            res = '{%d, {"%s", %d, %s, %s, %s' % (
                fnm["id"], fnm["name"], fnm["args_nums"], fnm["fn"], fnm["prepare"], fnm["close"])
        else:
            res ='{%d, {"%s", %d, %s' % (
                fnm["id"], fnm["name"], fnm["args_nums"], fnm["fn"])
        if "exception_safe" in fnm:
            res = res + ", " + str(fnm["exception_safe"])
            
        return res + "}}"

    value = dict()
    value["license"] = license_string
    value["functions"] = ", \n        ".join([gen_be_fn(i) for i in function_list])

    content = cpp_template.substitute(value)

    with open(path, mode="w+") as f:
        f.write(content)


if __name__ == '__main__':
    # Read the function metadata inputs
    for function in vectorized_functions.vectorized_functions:
        add_function(function)

    FE_PATH = "../../../../fe/fe-core/target/generated-sources/build/com/starrocks/builtins/"
    BE_PATH = "../../gen_cpp/opcode/"

    if not os.path.exists(FE_PATH):
        os.makedirs(FE_PATH)

    if not os.path.exists(BE_PATH):
        os.makedirs(BE_PATH)

    generate_fe(FE_PATH + "VectorizedBuiltinFunctions.java")
    generate_cpp(BE_PATH + "builtin_functions.cpp")
