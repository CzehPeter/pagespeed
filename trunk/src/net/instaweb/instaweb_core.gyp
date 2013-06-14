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
  'variables': {
    'instaweb_root': '../..',
    'chromium_code': 1,
    # Warning: duplicated from icu.gyp; so please remember to change both spots
    # if changing the default.
    'use_system_icu%': 0,
  },
  'targets': [
    {
      'target_name': 'instaweb_htmlparse_core',
      'type': '<(library)',
      'dependencies': [
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_html',
      ],
      'export_dependent_settings': [
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_html',
      ],
    },
    {
      'target_name': 'http_core',
      'type': '<(library)',
      'dependencies': [
        '<(DEPTH)/base/base.gyp:base',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_base_core',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_http',
      ],
      'sources': [
        'http/semantic_type.cc',
      ],
      'include_dirs': [
        '<(instaweb_root)',
        '<(DEPTH)',
      ],
      'direct_dependent_settings': {
        'include_dirs': [
          '<(instaweb_root)',
          '<(DEPTH)',
        ],
      },
    },
    {
      'target_name': 'instaweb_rewriter_html',
      'type': '<(library)',
      'dependencies': [
        'instaweb_htmlparse_core',
        '<(DEPTH)/base/base.gyp:base',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_base_core',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_http',
      ],
      'sources': [
        'rewriter/collapse_whitespace_filter.cc',
        'rewriter/elide_attributes_filter.cc',
        'rewriter/html_attribute_quote_removal.cc',
        'rewriter/remove_comments_filter.cc',
      ],
      'include_dirs': [
        '<(instaweb_root)',
        '<(DEPTH)',
      ],
      'direct_dependent_settings': {
        'include_dirs': [
          '<(instaweb_root)',
          '<(DEPTH)',
        ],
      },
      'export_dependent_settings': [
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_base_core',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_http',
        'instaweb_htmlparse_core',
      ]
    },
    {
      'target_name': 'html_minifier_main',
      'type': 'executable',
      'dependencies': [
        '<(DEPTH)/base/base.gyp:base',
        'instaweb_htmlparse_core',
        'instaweb_rewriter_html',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_base_core',
        '<(DEPTH)/pagespeed/kernel.gyp:pagespeed_http',
      ],
      'sources': [
        'rewriter/html_minifier_main.cc',
      ],
    },
  ],
  'conditions': [
    ['OS=="linux" and use_system_icu==0', {
      'targets': [
        # We build this target to make sure that we don't accidentially
        # introduce dependencies from the core libraries to non-core
        # libraries.
        {
          'target_name': 'html_minifier_main_dependency_check',
          'type': 'none',
          'dependencies': [
            'html_minifier_main',
          ],
          'actions': [
            {
              'action_name': 'html_minifier_main_dependency_check',
              'inputs': [
                '<(PRODUCT_DIR)/html_minifier_main',
              ],
              'outputs': [
                '<(PRODUCT_DIR)/html_minifier_main_dependency_check',
              ],
              'action': [
                'g++',
                '-pthread',
                '-o', '<@(_outputs)',
                '<(LIB_DIR)/html_minifier_main/net/instaweb/rewriter/html_minifier_main.o',

                # Only the following dependencies are allowed. If you
                # find yourself needing to add additional dependencies,
                # please check with bmcquade first.
                #
                # Note: these must be in dependency order to work; you can't
                # sort this list alphabetically.
                '<(LIB_DIR)/net/instaweb/libinstaweb_rewriter_html.a',
                '<(LIB_DIR)/pagespeed/libpagespeed_html.a',
                '<(LIB_DIR)/pagespeed/libpagespeed_html_gperf.a',
                '<(LIB_DIR)/pagespeed/libpagespeed_http.a',
                '<(LIB_DIR)/pagespeed/libpagespeed_base_core.a',
                '<(LIB_DIR)/build/temp_gyp/libgoogleurl.a',
                '<(LIB_DIR)/base/libbase.a',
                '<(LIB_DIR)/base/libbase_static.a',
                '<(LIB_DIR)/third_party/chromium/src/base/third_party/dynamic_annotations/libdynamic_annotations.a',
                '<(LIB_DIR)/third_party/modp_b64/libmodp_b64.a',
                '<(LIB_DIR)/third_party/icu/libicuuc.a',
                '<(LIB_DIR)/third_party/icu/libicudata.a',
                '-lrt',
              ],
            },
          ],
        },
      ],
    }],
  ],
}
