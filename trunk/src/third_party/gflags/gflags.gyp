# Copyright 2010 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

{
  'variables': {
    'gflags_gen_arch_root': 'gen/arch/<(OS)/<(target_arch)',
  },
  'targets': [
    {
      'target_name': 'gflags',
      'type': '<(library)',
      'direct_dependent_settings': {
        'include_dirs': [
          '<(gflags_gen_arch_root)/include', # To allow #include "gtest/gtest.h"
        ],
      },
      'include_dirs': [
        '<(gflags_gen_arch_root)/include',
        '<(gflags_gen_arch_root)/src', # For config.h
        ],
      'sources': [
        'src/gflags.cc',
      ],
    },
  ],
}
