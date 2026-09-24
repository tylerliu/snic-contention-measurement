#
# Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright notice, this list of
#       conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice, this list of
#       conditions and the following disclaimer in the documentation and/or other materials
#       provided with the distribution.
#     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
#       to endorse or promote products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

project('DOCA_SECURE_CHANNEL', 'C',
	version: '1.0.0',
	license: 'BSD-3',
	default_options: [
		'buildtype=debug',
		'c_std=c11'
	],
	meson_version: '>= 0.61.2'
)

add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: 'c')
add_project_arguments('-D _GNU_SOURCE', language: 'c')

# Dependencies
app_dependencies = []
app_dependencies += dependency('doca-common')
app_dependencies += dependency('doca-argp')
app_dependencies += dependency('doca-comch')
app_dependencies += dependency('threads')

# Check for strlcpy (libbsd)
c_compiler = meson.get_compiler('c')
if not c_compiler.has_function('strlcpy', prefix: '#include <string.h>')
	dependency_libbsd = dependency('libbsd', required: false)
	if dependency_libbsd.found()
		app_dependencies += dependency_libbsd
		add_project_arguments('-D DOCA_USE_LIBBSD', language: 'c')
		add_project_arguments('-D RTE_USE_LIBBSD', language: 'c')
	endif
endif

app_inc_dirs = [
	include_directories('.'),
]

app_srcs = [
	'secure_channel_core.c',
	'comch_utils.c',
	'utils.c',
	'common.c',
	'secure_channel.c',
]

executable('doca_secure_channel',
	app_srcs,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs)
