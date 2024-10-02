source /opt/rh/gcc-toolset-9/enable

nuke_version=14.0
revision=7
nuke_include="/opt/Nuke$nuke_version""v$revision/include"

build="build/Nuke$nuke_version"
mkdir -p $build

for cpp in $(ls plugins); do
    so="${cpp%.cpp}.so"
    gcc -shared -fPIC -I$nuke_include -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=0 -o $build/$so plugins/$cpp
done
