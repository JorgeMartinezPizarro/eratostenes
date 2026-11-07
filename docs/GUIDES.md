## Install primesieve 

To install the latest version (v12.17) from primesieve, use:

```sh
rm -rf /tmp/primesieve && git clone --depth 1 --branch v12.17 https://github.com/kimwalisch/primesieve.git /tmp/primesieve
cmake -S /tmp/primesieve -B /tmp/primesieve/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_PRIMESIEVE=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF
cmake --build /tmp/primesieve/build -j"$(nproc)"
sudo install -m755 /tmp/primesieve/build/primesieve /usr/local/bin/primesieve && rm -rf /tmp/primesieve
hash -r && primesieve --version
```
