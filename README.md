# LumeDemo
A minimal Lume 3D demo.
## Requirements
- Operating System: Windows 11 x64 (tested)
- CMake: 3.24 or later
- Build Tools: [Visual Studio 2022 Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe) (choose the **Desktop development with C++** workload)
- [Git SSH authentication](https://github.com/settings/keys) must be set up for submodule access (HTTPS connections often time out)
## Compile
Clone this repository:
```powershell
git clone git@github.com:jsnchng/LumeDemo.git
```
```powershell
cd LumeDemo
```
```powershell
git submodule update --init
```
Configure:
```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
```
Build:
```powershell
cmake --build build --config Release
```
## Usage
Using PowerShell:
```powershell
Push-Location build\Release; .\LumeDemo.exe; Pop-Location
```
Or **cd .\build\Release** and run LumeDemo.
