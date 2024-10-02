source /opt/rh/gcc-toolset-9/enable

nuke_include='/opt/Nuke14.0v7/include'
mkdir build

for cpp in $(ls plugins); do
    so="${cpp%.cpp}.so"
    gcc -shared -fPIC -I$nuke_include -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=0 -o build/$so plugins/$cpp
done

