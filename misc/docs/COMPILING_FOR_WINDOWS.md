## Compiling for Windows

### Cross compiling from Linux WSL

You can use the new Ubuntu Bash feature in Windows 10 to compile ezQuake for Windows.

To enable Bash for Windows, press the `Start` button and type `Turn Windows f` and select `Turn Windows features on or off`. Scroll down to `Windows Subsystem for Linux (Beta)` and enable it.

Now press WINDOWS+I, go to `Update & security` and then to the `For developers` tab. Enable `Developer mode`.

Now press the `Start` button again and enter `bash`. Click it and install Bash.

Enter the following command to install all required prerequisites to build ezQuake:

```
sudo apt-get install -y \
  autoconf automake libtool pkg-config curl zip unzip tar \
  cmake ninja-build mingw-w64
```

Now clone the ezQuake source code:

```
git clone https://github.com/ezQuake/ezquake-source.git ezquake
```

Initialize the build and compile dependencies:
```
cd ezquake
./bootstrap.sh
cmake --preset mingw64-x64-cross
```
This will take some time the first invocation, but is a one-off cost.

The dependency build cache is found in `${HOME}/.cache/vcpkg` if you
don't intend to build again and want to reclaim some space.

Now build the ezQuake executable:
```
cmake --build build-mingw64-x64-cross --config Release
```

This will produce `ezquake.exe` in the current directory. 