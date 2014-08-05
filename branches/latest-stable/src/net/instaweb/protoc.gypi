# Copyright 2009 Google Inc.
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
  'type': '<(library)',
  'rules': [
    {
      'rule_name': 'genproto',
      'extension': 'proto',
      'inputs': [
        '<(protoc_executable)',
      ],
      'message': 'Generating C++ code from <(RULE_INPUT_PATH)',
      'outputs': [
        '<(protoc_out_dir)/<(instaweb_protoc_subdir)/<(RULE_INPUT_ROOT).pb.h',
        '<(protoc_out_dir)/<(instaweb_protoc_subdir)/<(RULE_INPUT_ROOT).pb.cc',
        '<(protoc_out_dir)/<(instaweb_protoc_subdir)/<(RULE_INPUT_ROOT).proto',
      ],
      'action': [
        'bash',
        '-c',
        'cat <(instaweb_root)/<(instaweb_protoc_subdir)/<(RULE_INPUT_NAME) | sed \'s!"third_party/pagespeed!"pagespeed!\' | sed \'s!// \[opensource\] !!\' > <(protoc_out_dir)/<(instaweb_protoc_subdir)/<(RULE_INPUT_ROOT).proto && <(PRODUCT_DIR)/<(EXECUTABLE_PREFIX)protoc<(EXECUTABLE_SUFFIX) --proto_path=<(protoc_out_dir)/ <(protoc_out_dir)/<(instaweb_protoc_subdir)/<(RULE_INPUT_ROOT).proto --cpp_out=<(protoc_out_dir)',
      ],
    },
  ],
  'dependencies': [
    '<(DEPTH)/pagespeed/kernel.gyp:proto_util',
    '<(DEPTH)/third_party/protobuf/protobuf.gyp:protoc#host',
  ],
  'include_dirs': [
    '<(protoc_out_dir)',
    '<(DEPTH)',
  ],
  'export_dependent_settings': [
    '<(DEPTH)/pagespeed/kernel.gyp:proto_util',
  ],
  'hard_dependency': 1,
  'all_dependent_settings': {
    'hard_dependency': 1,
    'include_dirs': [
      '<(protoc_out_dir)',
      '<(DEPTH)/third_party/protobuf/src',
    ],
  },
}
