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

Open Visual Studio 2022 and select _Clone a repository_.

#### Cross-compiling from Linux or Ubuntu Bash (WSL)

See [misc/docs/COMPILING_FOR_WINDOWS.md](misc/docs/COMPILING_FOR_WINDOWS.md)

### Compiling a Linux binary

_These instructions were tested on Ubuntu_

Make sure you have the dependencies installed:

(You may skip installing dependencies if you are going to use the build-linux.sh script included in the repository, this will take care of installing packages for you.)

- For *Debian/Ubuntu 16.10+*
```
sudo apt install git build-essential libsdl2-2.0-0 libsdl2-dev libjansson-dev libexpat1-dev libcurl4-openssl-dev libpng-dev libjpeg-dev libspeex-dev libspeexdsp-dev libfreetype6-dev libsndfile1-dev libpcre2-dev libminizip-dev
```
- For *openSUSE Tumbleweed*
```
sudo zypper install -t pattern devel_C_C++
sudo zypper install git pcre2-devel Mesa-libGL-devel SDL2-devel libjansson-devel libexpat-devel libcurl-devel libpng16-devel libpng16-compat-devel libjpeg8-devel libjpeg-turbo libsndfile-devel speex-devel speexdsp-devel libXxf86vm-devel
```
- For *Fedora*
```
sudo dnf group install 'C Development Tools and Libraries'
sudo dnf install git pcre2-devel mesa-libEGL-devel SDL2-devel jansson-devel expat-devel libcurl-devel libpng-devel libjpeg-turbo-devel libsndfile-devel speex-devel speexdsp-devel libXxf86vm-devel
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
git submodule update --init --recursive --remote
```

Run the compilation (replace 5 with the number of cpu cores you have +1):
```
make -j5
```
You can add `-jN` as a parameter to `make` to build in parallell. Use number of cpu cores plus 1 (e.g. `-j5` if you have a quad core processor).

Copy the compiled binary to your Quake folder, on 64bit linux the binary will be called `ezquake-linux-x86_64`.

### Compiling an OS X binary

See [misc/docs/COMPILING_ON_OSX.md](misc/docs/COMPILING_ON_OSX.md)

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
