# Native Linux build. Run inside the Steam Runtime 3 SDK container for releases.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo"
if [[ "${1:-}" == "--install-tools" ]]; then
  apt-get update
  apt-get install -y binutils ca-certificates g++ git python3 python3-pip
fi
python3 setup.py
python3 -m pip install --target tools/ambuild 'git+https://github.com/alliedmodders/ambuild@d89ec91a7ac2607da07b50bb62346f9a10e9a998'
export PYTHONPATH="$repo/tools/ambuild${PYTHONPATH:+:$PYTHONPATH}"
export CC=gcc CXX=g++
mkdir -p obj
cd obj
python3 ../configure.py --sdks=cs2 --targets=x86_64 --enable-optimize --mms_path=../deps/metamod-source --hl2sdk-root=../deps --hl2sdk-manifests=../deps/metamod-source/hl2sdk-manifests
python3 -c 'from ambuild2.run import cli_run; cli_run()'
cd "$repo"
g++ -std=c++17 -I src -I deps/json tests/rules_test.cpp -o tools/rules_test-linux
tools/rules_test-linux
g++ -std=c++17 -I src tests/smoke_test.cpp -o tools/smoke_test-linux
tools/smoke_test-linux
g++ -std=c++17 -I src tests/targeting_test.cpp -o tools/targeting_test-linux
tools/targeting_test-linux
g++ -std=c++17 -I src -I deps/json tests/vis_test.cpp -o tools/vis_test-linux
tools/vis_test-linux
g++ -std=c++17 -I src tests/smoke_visibility_test.cpp -o tools/smoke_visibility_test-linux
tools/smoke_visibility_test-linux
g++ -std=c++17 -I src tests/m_visibility_test.cpp -o tools/m_visibility_test-linux
tools/m_visibility_test-linux
for test in gamedata_test update_policy_test map_update_test; do
  g++ -std=c++17 -I src -I deps/json "tests/$test.cpp" -o "tools/$test-linux"
  "tools/$test-linux"
done

python3 check_abi.py obj/package/cs2/addons/antiwallhack/bin/linuxsteamrt64/antiwallhack.so
python3 package.py --build obj/package/cs2
