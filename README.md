# ezQuake — Modern QuakeWorld Client
Homepage: [https://ezquake.com][homepage]

Community discord: [http://discord.quake.world][discord]

This is the right place to start playing QuakeWorld&reg; — the fastest first
person shooter action game ever.

Combining the features of all modern QuakeWorld® clients, ezQuake makes
QuakeWorld&reg; easier to start and play. The immortal first person shooter
Quake&reg; in the brand new skin with superb graphics and extremely fast
gameplay.

## Features

 * Modern graphics
 * [QuakeTV][qtv] support
 * Rich menus
 * Multiview support
 * Tons of features to serve latest pro-gaming needs
 * Built in server browser & MP3 player control
 * Recorded games browser
 * Customization of all possible graphics elements of the game including Heads Up Display
 * All sorts of scripting possibilities
 * Windows, Linux, MacOSX and FreeBSD platforms supported (SDL2).

Our client comes only with bare minimum of game media. If you want to
experience ezQuake with modern graphics and other additional media including
custom configurations, maps, textures and more, try using the [nQuake][nQuake]-installer.

## Support

Need help with using ezQuake? Try #dev-corner on [discord][discord]

Or (less populated these days) visit us on IRC at QuakeNet, channel #ezQuake: [webchat][webchat] or [IRC][IRC].

Sometimes help from other users of ezQuake might be more useful to you so you
can also try visiting the [quakeworld.nu Client Talk-forums][forum].

If you have found a bug, please report it [here][issues]

## Installation guide

To play Quakeworld you need the files *pak0.pak* and *pak1.pak* from the original Quake-game.

### Install ezQuake to an existing Quake-installation
If you have an existing Quake-installation simply extract the ezQuake executable into your Quake-directory.

A typical error message when installing ezQuake into a pre-existing directory is about *glide2x.dll* missing.
To get rid of this error, remove the file *opengl32.dll* from your Quake directory.

### Upgrade an nQuake-installation
If you have a version of [nQuake][nQuake] already installed you can upgrade ezQuake by extracting the new executable into the nQuake-directory.

### Minimal clean installation
If you want to make a clean installation of ezQuake you can do this by following these steps:

1. Create a new directory
2. Extract the ezQuake-executable into this directory
3. Create a subdirectory called *id1*
4. Copy *pak0.pak* and *pak1.pak* into this subdirectory

## Compiling

### Compiling a Windows binary

#### Using Visual Studio
The project contain a VS solution.

Clone the ezQuake source code:

```
git clone --recurse-submodules https://github.com/ezQuake/ezquake-source.git ezquake
```

Initialize Vcpkg, and refresh submodules if needed by invoking the `bootstrap.ps1` script via right click menu.

Visual Studio has native CMake support so simply load the project and compile your preferred target.

If you instead prefer a Visual Studio Solution, CMake can generate this via:

```
cmake --preset msbuild-x64
```

The solution is found under `cmake-build-presets/msbuild-x64/`.

#### Using Ubuntu WSL or Native

You can use the new Ubuntu Bash feature in Windows 10 to compile ezQuake for Windows.

To enable Bash for Windows, press the `Start` button and type `Turn Windows f` and select `Turn Windows features on or off`. Scroll down to `Windows Subsystem for Linux (Beta)` and enable it.

Now press WINDOWS+I, go to `Update & security` and then to the `For developers` tab. Enable `Developer mode`.

Now press the `Start` button again and enter `bash`. Click it and install Bash.

Enter the following command to install all required prerequisites to build ezQuake:

```
sudo apt-get install -y git mingw-w64 build-essential cmake ninja-build curl zip autoconf libtool
```

Now clone the ezQuake source code:

```
git clone --recurse-submodules https://github.com/ezQuake/ezquake-source.git ezquake
```

Make sure line endings are not CRLF:

```
dos2unix *.sh
```

Now build the ezQuake executable:

```
./bootstrap.sh # initialize vcpkg helper

# Configure and build all 3rd party dependencies, grab a beverage of choice.
cmake --preset ninja-mingw64-x64 # or 'ninja-mingw64-i686'
cmake --build cmake-build-presets/ninja-mingw64-x64 --config Release
```
If cross-compiling from Linux the targets are called `ninja-mingw64-x64-cross` and `ninja-mingw64-i686`.

Copy the compiled binary to your Quake folder, the binary is called `cmake-build-presets/ninja-mingw64-x64/Release/ezquake.exe`.

### Compiling a Linux binary

_These instructions were tested on Ubuntu_

Make sure you have the dependencies installed:

(You may skip installing dependencies if you are going to use the build-linux.sh script included in the repository, this will take care of installing packages for you.)

- For *Debian/Ubuntu 16.10+*
```
sudo apt install cmake ninja-build git build-essential libsdl2-2.0-0 libsdl2-dev libjansson-dev libexpat1-dev libcurl4-openssl-dev libpng-dev libjpeg-dev libspeex-dev libspeexdsp-dev libfreetype6-dev libsndfile1-dev libpcre2-dev libminizip-dev
```
- For *openSUSE Tumbleweed*
```
sudo zypper install -t pattern devel_C_C++
sudo zypper install cmake ninja-build git pcre2-devel Mesa-libGL-devel SDL2-devel libjansson-devel libexpat-devel libcurl-devel libpng16-devel libpng16-compat-devel libjpeg8-devel libjpeg-turbo libsndfile-devel speex-devel speexdsp-devel libXxf86vm-devel
```
- For *Fedora*
```
sudo dnf group install 'C Development Tools and Libraries'
sudo dnf install cmake ninja-build git pcre2-devel mesa-libEGL-devel SDL2-devel jansson-devel expat-devel libcurl-devel libpng-devel libjpeg-turbo-devel libsndfile-devel speex-devel speexdsp-devel libXxf86vm-devel
```

Clone the git repository:
```
git clone --recurse-submodules https://github.com/ezQuake/ezquake-source.git
```

Switch to `ezquake-source` path:
```
cd ~/ezquake-source/
```

Initialize/update git submodules:
```
cmake --preset ninja-generic-dynamic # tweak flags via -DCMAKE_C_FLAGS="-march=native"
cmake --build cmake-build-presets/ninja-generic-dynamic --config Release
```

Copy the compiled binary to your Quake folder, the binary is called `cmake-build-presets/ninja-generic-dynamic/Release/ezquake`.

If you instead wish to build a static binary, install `autoconf`, `automake`, and `libtool` and run:

```
./bootstrap.sh # initialize vcpkg helper
# Configure and build all 3rd party dependencies, grab a beverage of choice.
cmake --preset ninja-generic-static # tweak flags via -DCMAKE_C_FLAGS="-march=native"
cmake --build cmake-build-presets/ninja-generic-static --config Release
```

### Compiling a macOS 11.0+ binary


Get [Homebrew](http://brew.sh)

Run exactly as it says on the front page:

```
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Make sure you run the `brew doctor` as instructed before doing anything else.

Then run:

```
brew install autoconf automake libtool
```

When it's done, run:

```
./bootstrap.sh # initialize vcpkg helper
# Configure and build all 3rd party dependencies, grab a beverage of choice.
cmake --preset xcode-arm64 # or xcode-x64
cmake --build cmake-build-presets/xcode-arm64 --config Release
```

App bundle is called `cmake-build-presets/xcode-arm64/Release/ezQuake.app`.

## Nightly builds

Nightly builds can be found [here][nightly]

 [nQuake]: http://nquake.com/
 [webchat]: http://webchat.quakenet.org/?channels=#ezquake
 [IRC]: irc://irc.quakenet.org/#ezquake
 [forum]: http://www.quakeworld.nu/forum/8
 [qtv]: http://qtv.quakeworld.nu/
 [nightly]: https://builds.quakeworld.nu/ezquake/snapshots/
 [releases]: https://github.com/ezQuake/ezquake-source/releases
 [issues]: https://github.com/ezQuake/ezquake-source/issues
 [homepage]: https://ezquake.com
 [discord]: http://discord.quake.world/
