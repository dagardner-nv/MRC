#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2022-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

rapids-logger "Env Setup"
source /opt/conda/etc/profile.d/conda.sh
export MRC_ROOT=${MRC_ROOT:-$(git rev-parse --show-toplevel)}
cd ${MRC_ROOT}
# For non-gpu hosts nproc will correctly report the number of cores we are able to use
# On a GPU host however nproc will report the total number of cores and PARALLEL_LEVEL
# will be defined specifying the subset we are allowed to use.
NUM_CORES=$(nproc)
export PARALLEL_LEVEL=${PARALLEL_LEVEL:-${NUM_CORES}}
rapids-logger "Procs: ${NUM_CORES}"
rapids-logger "Memory"

/usr/bin/free -g

rapids-logger "user info"
id

# NUM_PROC is used by some of the other scripts
export NUM_PROC=${PARALLEL_LEVEL:-$(nproc)}
export BUILD_CC=${BUILD_CC:-"gcc"}

export CONDA_ENV_YML="${MRC_ROOT}/ci/conda/environments/dev_env.yml"
export CONDA_CLANG_ENV_YML="${MRC_ROOT}/ci/conda/environments/clang_env.yml"
export CONDA_CI_ENV_YML="${MRC_ROOT}/ci/conda/environments/ci_env.yml"

export CMAKE_BUILD_ALL_FEATURES="-DCMAKE_MESSAGE_CONTEXT_SHOW=ON -DMRC_BUILD_BENCHMARKS=ON -DMRC_BUILD_EXAMPLES=ON -DMRC_BUILD_PYTHON=ON -DMRC_BUILD_TESTS=ON -DMRC_USE_CONDA=ON -DMRC_PYTHON_BUILD_STUBS=ON"
export CMAKE_BUILD_WITH_CODECOV="-DCMAKE_BUILD_TYPE=Debug -DMRC_ENABLE_CODECOV=ON -DMRC_PYTHON_PERFORM_INSTALL:BOOL=ON -DMRC_PYTHON_INPLACE_BUILD:BOOL=ON"

# Set the depth to allow git describe to work
export GIT_DEPTH=1000

# For PRs, $GIT_BRANCH is like: pull-request/989
REPO_NAME=$(basename "${GITHUB_REPOSITORY}")
ORG_NAME="${GITHUB_REPOSITORY_OWNER}"
PR_NUM="${GITHUB_REF_NAME##*/}"


# S3 vars
export S3_URL="s3://rapids-downloads/ci/mrc"
export DISPLAY_URL="https://downloads.rapids.ai/ci/mrc"
export ARTIFACT_ENDPOINT="/pull-request/${PR_NUM}/${GIT_COMMIT}/${NVARCH}/${BUILD_CC}"
export ARTIFACT_URL="${S3_URL}${ARTIFACT_ENDPOINT}"
export DISPLAY_ARTIFACT_URL="${DISPLAY_URL}${ARTIFACT_ENDPOINT}"

# Set sccache env vars
export SCCACHE_S3_KEY_PREFIX=mrc-${NVARCH}-${BUILD_CC}
export SCCACHE_BUCKET=rapids-sccache-east
export SCCACHE_REGION="us-east-2"
export SCCACHE_IDLE_TIMEOUT=32768
#export SCCACHE_LOG=debug

mkdir -p ${WORKSPACE_TMP}

function print_env_vars() {
    rapids-logger "Environ:"
    env | grep -v -E "AWS_ACCESS_KEY_ID|AWS_SECRET_ACCESS_KEY|TOKEN" | sort
}

function update_conda_env() {
    rapids-logger "Checking for updates to conda env"

    # Deactivate the environment first before updating
    conda deactivate

    if [[ "${SKIP_CONDA_ENV_UPDATE}" == "" ]]; then
        # Make sure we have the conda-merge package installed
        if [[ -z "$(conda list | grep conda-merge)" ]]; then
            rapids-mamba-retry install -q -n mrc -c conda-forge "conda-merge>=0.2"
        fi
    fi

    # Create a temp directory which we store the combined environment file in
    condatmpdir=$(mktemp -d)

    # Merge the environments together so we can use --prune. Otherwise --prune
    # will clobber the last env update
    conda run -n mrc --live-stream conda-merge ${CONDA_ENV_YML} ${CONDA_CLANG_ENV_YML} ${CONDA_CI_ENV_YML} > ${condatmpdir}/merged_env.yml

    if [[ "${SKIP_CONDA_ENV_UPDATE}" == "" ]]; then
        # Update the conda env with prune remove excess packages (in case one was removed from the env)
        rapids-mamba-retry env update -n mrc --prune --file ${condatmpdir}/merged_env.yml
    fi

    # Delete the temp directory
    rm -rf ${condatmpdir}

    # Finally, reactivate
    conda activate mrc

    rapids-logger "Final Conda Environment"
    conda list
}

print_env_vars

function fetch_base_branch_gh_api() {
    # For PRs, $GIT_BRANCH is like: pull-request/989
    REPO_NAME=$(basename "${GITHUB_REPOSITORY}")
    ORG_NAME="${GITHUB_REPOSITORY_OWNER}"
    PR_NUM="${GITHUB_REF_NAME##*/}"

    rapids-logger "Retrieving base branch from GitHub API"
    [[ -n "$GH_TOKEN" ]] && CURL_HEADERS=('-H' "Authorization: token ${GH_TOKEN}")
    RESP=$(
    curl -s \
        -H "Accept: application/vnd.github.v3+json" \
        "${CURL_HEADERS[@]}" \
        "${GITHUB_API_URL}/repos/${ORG_NAME}/${REPO_NAME}/pulls/${PR_NUM}"
    )

    export BASE_BRANCH=$(echo "${RESP}" | jq -r '.base.ref')

    # Change target is the branch name we are merging into but due to the weird way jenkins does
    # the checkout it isn't recognized by git without the origin/ prefix
    export CHANGE_TARGET="origin/${BASE_BRANCH}"
}

function fetch_base_branch_local() {
    rapids-logger "Retrieving base branch from git"
    git remote add upstream ${GIT_UPSTREAM_URL}
    git fetch upstream --tags
    source ${MRC_ROOT}/ci/scripts/common.sh
    export BASE_BRANCH=$(get_base_branch)
    export CHANGE_TARGET="upstream/${BASE_BRANCH}"
}

function fetch_base_branch() {
    if [[ "${LOCAL_CI}" == "1" ]]; then
        fetch_base_branch_local
    else
        fetch_base_branch_gh_api
    fi

    git submodule update --init --recursive
    rapids-logger "Base branch: ${BASE_BRANCH}"
}

function fetch_s3() {
    ENDPOINT=$1
    DESTINATION=$2
    if [[ "${USE_S3_CURL}" == "1" ]]; then
        curl -f "${DISPLAY_URL}${ENDPOINT}" -o "${DESTINATION}"
        FETCH_STATUS=$?
    else
        aws s3 cp --no-progress "${S3_URL}${ENDPOINT}" "${DESTINATION}"
        FETCH_STATUS=$?
    fi
}

function show_conda_info() {

    rapids-logger "Check Conda info"
    conda info
    conda config --show-sources
    conda list --show-channel-urls
}

function upload_artifact() {
    FILE_NAME=$1
    BASE_NAME=$(basename "${FILE_NAME}")
    rapids-logger "Uploading artifact: ${BASE_NAME}"
    if [[ "${LOCAL_CI}" == "1" ]]; then
        cp ${FILE_NAME} "${LOCAL_CI_TMP}/${BASE_NAME}"
    else
        aws s3 cp --only-show-errors "${FILE_NAME}" "${ARTIFACT_URL}/${BASE_NAME}"
        echo "- ${DISPLAY_ARTIFACT_URL}/${BASE_NAME}" >> ${GITHUB_STEP_SUMMARY}
    fi
}
