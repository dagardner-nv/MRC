#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

source ${WORKSPACE}/ci/scripts/jenkins/common.sh

gpuci_logger "Downloading build artifacts from ${DISPLAY_ARTIFACT_URL}"
aws s3 cp --no-progress "${ARTIFACT_URL}/conda_env.tar.gz" "${WORKSPACE_TMP}/conda_env.tar.gz"
aws s3 cp --no-progress "${ARTIFACT_URL}/workspace.tar.bz" "${WORKSPACE_TMP}/workspace.tar.bz"

gpuci_logger "Extracting"
mkdir -p /opt/conda/envs/morpheus
tar xf "${WORKSPACE_TMP}/conda_env.tar.gz" --no-same-owner --directory /opt/conda/envs/morpheus
tar xf "${WORKSPACE_TMP}/workspace.tar.bz" --no-same-owner

gpuci_logger "Setting test env"
conda activate morpheus
conda-unpack

# Work-around for issue where libmorpheus_utils.so is not found by libmorpheus.so
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:${MORPHEUS_ROOT}/morpheus/_lib

pip install -e ${MORPHEUS_ROOT}

cd ${MORPHEUS_ROOT}/docs
gpuci_logger "Installing Documentation dependencies"
pip install -r requirement.txt

gpuci_logger "Building docs"
make html

gpuci_logger "Tarring the docs"
tar cfj "${WORKSPACE_TMP}/docs.tar.bz" build/html

gpuci_logger "Pushing results to ${DISPLAY_ARTIFACT_URL}"
aws s3 cp --no-progress "${WORKSPACE_TMP}/docs.tar.bz" "${ARTIFACT_URL}/docs.tar.bz"

gpuci_logger "Success"
exit 0
