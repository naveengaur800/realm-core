/*************************************************************************
 *
 * Copyright 2022 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef FUZZ_ENGINE_HPP
#define FUZZ_ENGINE_HPP

#include <vector>
#include <string>
#include <iostream>
#include <cstring>
#include <fstream>
#include <cstdlib>
#if REALM_USE_UV
#include <uv.h>
#endif

class FuzzConfigurator;
class FuzzEngine {
public:
    int run_fuzzer(const std::string& input, const std::string& name, bool = false, const std::string& = "");

private:
    void do_fuzz(FuzzConfigurator&);
};

#endif
