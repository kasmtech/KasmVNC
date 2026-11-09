# The private GLIBC 2.28-compatible libgcc and its build environment are cached
# independently. CI supplies an immutable image digest, never a moving tag.
ARG GCC_RUNTIME_IMAGE
FROM ${GCC_RUNTIME_IMAGE}
ARG DEPENDENCY_CONTEXT_SHA
LABEL org.kasmweb.portable-dependency-key=$DEPENDENCY_CONTEXT_SHA
COPY . /opt/kasm-dependency-recipes/
ENV BUILD_JOBS=8
RUN test -n "$DEPENDENCY_CONTEXT_SHA" && /opt/kasm-dependency-recipes/install_sccache_from_github && /opt/kasm-dependency-recipes/build
