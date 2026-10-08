## WSL issues

WSL2's virtual disk doesn't shrink back automatically after deleting large files inside it. From PowerShell:

```powershell
Get-ChildItem "$env:LOCALAPPDATA\Packages" -Filter *.vhdx -Recurse | Select-Object FullName, Length
wsl --shutdown
```
Then open ```diskpart``` and add 

```powershell
select vdisk file="C:\ruta\completa\a\ext4.vhdx"
compact vdisk
```

## Install primesieve 

To install the latest version (v12.16) from primesieve, use:

```sh
rm -rf /tmp/primesieve && git clone --depth 1 --branch v12.16 https://github.com/kimwalisch/primesieve.git /tmp/primesieve
cmake -S /tmp/primesieve -B /tmp/primesieve/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_PRIMESIEVE=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF
cmake --build /tmp/primesieve/build -j"$(nproc)"
sudo install -m755 /tmp/primesieve/build/primesieve /usr/local/bin/primesieve && rm -rf /tmp/primesieve
hash -r && primesieve --version
```
