# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.15

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /Applications/CLion.app/Contents/bin/cmake/mac/bin/cmake

# The command to remove a file.
RM = /Applications/CLion.app/Contents/bin/cmake/mac/bin/cmake -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug

# Include any dependencies generated for this target.
include CMakeFiles/ffplay.dir/depend.make

# Include the progress variables for this target.
include CMakeFiles/ffplay.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/ffplay.dir/flags.make

CMakeFiles/ffplay.dir/ffplay.c.o: CMakeFiles/ffplay.dir/flags.make
CMakeFiles/ffplay.dir/ffplay.c.o: ../ffplay.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building C object CMakeFiles/ffplay.dir/ffplay.c.o"
	/Library/Developer/CommandLineTools/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/ffplay.dir/ffplay.c.o   -c /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/ffplay.c

CMakeFiles/ffplay.dir/ffplay.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/ffplay.dir/ffplay.c.i"
	/Library/Developer/CommandLineTools/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/ffplay.c > CMakeFiles/ffplay.dir/ffplay.c.i

CMakeFiles/ffplay.dir/ffplay.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/ffplay.dir/ffplay.c.s"
	/Library/Developer/CommandLineTools/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/ffplay.c -o CMakeFiles/ffplay.dir/ffplay.c.s

CMakeFiles/ffplay.dir/cmdutils.c.o: CMakeFiles/ffplay.dir/flags.make
CMakeFiles/ffplay.dir/cmdutils.c.o: ../cmdutils.c
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building C object CMakeFiles/ffplay.dir/cmdutils.c.o"
	/Library/Developer/CommandLineTools/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -o CMakeFiles/ffplay.dir/cmdutils.c.o   -c /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmdutils.c

CMakeFiles/ffplay.dir/cmdutils.c.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing C source to CMakeFiles/ffplay.dir/cmdutils.c.i"
	/Library/Developer/CommandLineTools/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -E /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmdutils.c > CMakeFiles/ffplay.dir/cmdutils.c.i

CMakeFiles/ffplay.dir/cmdutils.c.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling C source to assembly CMakeFiles/ffplay.dir/cmdutils.c.s"
	/Library/Developer/CommandLineTools/usr/bin/cc $(C_DEFINES) $(C_INCLUDES) $(C_FLAGS) -S /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmdutils.c -o CMakeFiles/ffplay.dir/cmdutils.c.s

CMakeFiles/ffplay.dir/FFplayer.cpp.o: CMakeFiles/ffplay.dir/flags.make
CMakeFiles/ffplay.dir/FFplayer.cpp.o: ../FFplayer.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Building CXX object CMakeFiles/ffplay.dir/FFplayer.cpp.o"
	/Library/Developer/CommandLineTools/usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/ffplay.dir/FFplayer.cpp.o -c /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/FFplayer.cpp

CMakeFiles/ffplay.dir/FFplayer.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/ffplay.dir/FFplayer.cpp.i"
	/Library/Developer/CommandLineTools/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/FFplayer.cpp > CMakeFiles/ffplay.dir/FFplayer.cpp.i

CMakeFiles/ffplay.dir/FFplayer.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/ffplay.dir/FFplayer.cpp.s"
	/Library/Developer/CommandLineTools/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/FFplayer.cpp -o CMakeFiles/ffplay.dir/FFplayer.cpp.s

# Object files for target ffplay
ffplay_OBJECTS = \
"CMakeFiles/ffplay.dir/ffplay.c.o" \
"CMakeFiles/ffplay.dir/cmdutils.c.o" \
"CMakeFiles/ffplay.dir/FFplayer.cpp.o"

# External object files for target ffplay
ffplay_EXTERNAL_OBJECTS =

ffplay: CMakeFiles/ffplay.dir/ffplay.c.o
ffplay: CMakeFiles/ffplay.dir/cmdutils.c.o
ffplay: CMakeFiles/ffplay.dir/FFplayer.cpp.o
ffplay: CMakeFiles/ffplay.dir/build.make
ffplay: CMakeFiles/ffplay.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Linking CXX executable ffplay"
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/ffplay.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/ffplay.dir/build: ffplay

.PHONY : CMakeFiles/ffplay.dir/build

CMakeFiles/ffplay.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/ffplay.dir/cmake_clean.cmake
.PHONY : CMakeFiles/ffplay.dir/clean

CMakeFiles/ffplay.dir/depend:
	cd /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug /Users/alexander/mydev/workspace_github/audio_video/ffmpeg_code/ffplay/cmake-build-debug/CMakeFiles/ffplay.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/ffplay.dir/depend

