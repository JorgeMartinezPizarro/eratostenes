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
