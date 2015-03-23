
This project consists of several bug-finding tools that look for memory
protection errors in the C source code of the [GNU
R](http://www.r-project.org/) core and packages.  

As input the tools need the LLVM bitcode of the R binary. To check an R
package, the tools also need the bitcode of the shared library of that
package.  To get these bitcode files one needs to build R with the
[CLANG](http://clang.llvm.org/) compiler (more instructions available
[here](BUILDING.md)).  For convenience of playing with the tools, the bitcode
of the R binary from R-devel version 67741 generated by CLANG/LLVM 4.2.0 is
included in the `examples` directory.

The tools need [LLVM 3.5.0](http://llvm.org/releases/download.html) to build
(one can download one of the CLANG/LLVM [pre-built
binaries](http://llvm.org/releases/download.html#3.5.0)).  One then needs to
modify the LLVM installation path in `src/Makefile` and run `make`.

We are fixing the bugs in [R-devel](https://svn.r-project.org/R/trunk/),
both in the core R and in the packages that are part of the distribution. 
We have not been fixing the CRAN/BIOC packages, where a number of errors can
be found as well.  We are happy to give advice to interested package
maintainers on how to use the tool.

A high-level description of the tools is available [here](USAGE.md).
