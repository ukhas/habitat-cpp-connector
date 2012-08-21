habitat cpp uploader (see habitat.uploader.Uploader)
====================================================

Building dependencies
---------------------

You will need these dependencies:

 - [JsonCpp](http://jsoncpp.sourceforge.net/)
 - [libcURL](http://curl.haxx.se/)
 - habitat (for habitat.utils.rfc3339, to test the Uploader.)

Both build from source fairly easily, but the easiest way to acquire them for
Ubuntu is:

 - [JsonCpp (PPA)](https://launchpad.net/~danieljonathanrichman/+archive/ppa)
   for Ubuntu oneiric (11.10) and older
 - [libjsoncpp-dev (apt)](http://packages.ubuntu.com/precise/libjsoncpp-dev)
 - [libcurl4-openssl-dev (apt)](http://packages.ubuntu.com/precise/libcurl4-openssl-dev)

Or Debian:

 - libjsoncpp-dev in wheezy and newer (the wheezy package also works fine on squeeze)
 - libcurl4-openssl-dev

