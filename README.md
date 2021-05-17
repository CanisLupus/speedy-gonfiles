Speedy GonFiles
=================

## Description

Speedy GonFiles is a fast Windows program to find duplicate files. Run the program and drag files/folders to the window. It will process all the files and mark any duplicates with colors.

Download [here](https://github.com/CanisLupus/speedy-gonfiles/releases).

It was heavily inspired by [HashMyFiles](https://www.nirsoft.net/utils/hash_my_files.html), which I frequently used to check for duplicates. However, instead of focusing on applying commonly known hashes and showing duplicate files as a by-product, Speedy GonFiles focuses on finding duplicates, so it only hashes files for comparison if multiple files have the same size (using the very fast [Meow hash](https://github.com/cmuratori/meow_hash)). It can also add files inside folders recursively, unlike HashMyFiles.

## Requirements

- Windows 10, 64 bits
- x64 CPU with AES-NI (Meow hash requirement)

NOTE: 32-bit Windows is NOT supported. Older Windows versions or somewhat old CPUs were not tested, so they may or may not work, or may simply become slow. Feel free to try if you want.

## Project

### Building

Open the solution (`project/Speedy GonFiles.sln`) in Visual Studio and run it. 😉 You need to have the Visual Studio module *Desktop development with C++* installed. I used Visual Studio 16.9.2.

### Code

The full program is mostly a single file, `project/src/program.cpp` (there are also external files for Meow hash, Timsort, and application resources). Although it's not my usual style, this project was small enough to comfortably develop and organize in one file. I also greatly prefer to read code written by others on as few files as it makes sense instead of searching for all behaviours and data in dozens of files, so hopefully you will understand. 😄

Although this is a C++ project and uses the STL, there are no new classes or "modern" C++ features. It's very C-based, using only structs and functions. There are no forward declarations, so a function can only use things above itself. The entry point (wWinMain) is thus the last function.

This is my first complete project using the Win32 API, so please forgive the inevitable non-standard code. I wanted to learn how to create most things from code, including menus, so only using resource files (.rc) to have the (default) application icon is intentional.

### Possible improvements

- Read all the file paths and sizes from NFTS records on the Master File Table. It would probably be *much* faster than calling Windows APIs once per file, despite also being much harder to implement. Programs like WizTree index millions of files in a few seconds on an SSD, so it should be doable.

- Add file icons. I actually have code for this but it proved extremely slow and I didn't research it further, so it was left defined-out. Maybe reading them in a separate thread would work better? Maybe there are just better ways...?

## Credits / Licenses

Speedy GonFiles was made by Daniel Lobo and is published under the zlib license.

[Meow hash](https://github.com/cmuratori/meow_hash) 0.5/calico, used in this project, was developed by Casey Muratori and Jacob Christian Munch-Andersen, with additional contributions from other people. It is provided via the zlib license.

[timsort](https://github.com/patperry/timsort), used in this project, was developed by Patrick O. Perry as a port of the Java/Android version of Timsort, which in turn is a port of the original for Python, by Tim Peters. It is provided via the Apache License, version 2.0.

###### Yes, Speedy GonFiles is a bad pun on Speedy Gonzáles.
