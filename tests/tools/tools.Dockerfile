# Tools image for Hyprnotes hardening: clang, clang-tidy, cppcheck, valgrind, lcov, strace, hyperfine.
# Built only through tools-container.sh image; the build downloads packages, runs use --network none.
FROM archlinux:latest
RUN pacman -Syu --noconfirm --needed base-devel cmake ninja git python python-pip \
      qt6-base qt6-svg qt6-wayland md4c sqlite lua libarchive libglvnd \
      clang compiler-rt llvm cppcheck valgrind lcov strace hyperfine fontconfig ttf-dejavu \
    && pacman -Scc --noconfirm
WORKDIR /work
# valgrind memcheck refuses to start without symbols for ld.so/libc (it must redirect memcmp etc.). Arch ships none, so
# fetch them from Arch's debuginfod at image build time into /usr/lib/debug/.build-id (build-time network only).
RUN pacman -Sy --noconfirm --needed debuginfod \
    && export DEBUGINFOD_URLS=https://debuginfod.archlinux.org \
    && for f in /usr/lib/ld-linux-x86-64.so.2 /usr/lib/libc.so.6; do \
         id=$(readelf -n $f | awk '/Build ID/{print $3}'); \
         p=$(debuginfod-find debuginfo $id) && d=/usr/lib/debug/.build-id/${id:0:2} \
         && mkdir -p $d && cp "$p" $d/${id:2}.debug || exit 1; \
       done \
    && pacman -Scc --noconfirm
