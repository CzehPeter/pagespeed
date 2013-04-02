# Copyright 2012 Google Inc.
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
    'jsoncpp_root': '<(DEPTH)/third_party/jsoncpp',
  },
  'targets': [
    {
      'target_name': 'jsoncpp',
      'type': '<(library)',
      'include_dirs': [
        '<(jsoncpp_root)/include',
      ],
      'sources': [
        'src/lib_json/json_batchallocator.h',
        'src/lib_json/json_internalarray.inl',
        'src/lib_json/json_internalmap.inl',
        'src/lib_json/json_reader.cpp',
        'src/lib_json/json_tool.h',
        'src/lib_json/json_value.cpp',
        'src/lib_json/json_valueiterator.inl',
        'src/lib_json/json_writer.cpp',
      ],
    },
  ],
}
