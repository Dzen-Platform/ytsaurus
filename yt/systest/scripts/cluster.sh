#!/bin/bash

set -e
set -x

script_name=$0
ytsaurus_source_path="."
namespace=""
image=""
systest_image=""

print_usage() {
    cat << EOF
Usage: $script_name [-h|--help]
                    [--ytsaurus-source-path /path/to/ytsaurus.repo]
                    [--image registry/path/to/image:tag]
                    [--systest-image registry/path/to/systest/image:tag]
                    [--namespace namespace]
EOF
    exit 1
}

while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        --ytsaurus-source-path)
        ytsaurus_source_path=$(realpath "$2")
        shift 2
        ;;
        --namespace)
        namespace="$2"
        shift 2
        ;;
        --image)
        image="$2"
        shift 2
        ;;
        --systest-image)
        systest_image="$2"
        shift 2
        ;;
        *)
        echo "Unknown argument $1"
        print_usage
        ;;
    esac
done

if [[ ${image} == "" ]]; then
echo "  --image is required"
rint_usage
fi

kubectl cluster-info

nsflags=""
tester_flags=""
name_cluster="ytsaurus"
name_tester="tester"
name_systest="systest"
name_new_stress_test="newstresstest"

if [[ ${namespace} != "" ]]; then
  kubectl create namespace ${namespace}
  nsflags="-n ${namespace}"
  tester_flags="--namespace ${namespace}"
  name_cluster="ytsaurus-${namespace}"
  name_tester="tester-${namespace}"
  name_systest="systest-${namespace}"
  name_new_stress_test="newstresstest-${namespace}"
fi

helm install ${nsflags} ${name_cluster} --set YtsaurusImagePath=${image} ${ytsaurus_source_path}/yt/systest/helm/cluster

# Wait for Cypress
helm install ${nsflags} ${name_tester} --set YtsaurusImagePath=${image} ${ytsaurus_source_path}/yt/systest/helm/tester
bash ${ytsaurus_source_path}/yt/systest/scripts/wait.sh --name tester ${tester_flags}

helm install ${nsflags} ${name_systest} --set SystestImagePath=${systest_image} ${ytsaurus_source_path}/yt/systest/helm/systest
bash ${ytsaurus_source_path}/yt/systest/scripts/wait.sh --wait-minutes 60  --name systest ${tester_flags}

# NOTE(orlovorlov) temporarily disable new_stress_test until a user job can run in a docker image. It needs numpy.
#helm install ${nsflags} ${name_new_stress_test} --set SystestImagePath=${systest_image} ${ytsaurus_source_path}/yt/systest/helm/new_stress_test
#bash ${ytsaurus_source_path}/yt/systest/scripts/wait.sh --wait-minutes 60  --name newstresstest ${tester_flags}
