ARG CORE_IMAGE=kasmweb/core-ubuntu-noble:1.19.0-rolling-weekly
FROM ${CORE_IMAGE}
USER root
ARG PACKAGE
ADD runtime.tar.xz /
COPY core-entrypoint.sh /usr/local/bin/nix-core-entrypoint
RUN test -x "$PACKAGE/bin/Xkasmvnc" && \
    for command in Xkasmvnc Xvnc kasmvncserver vncserver kasmvncpasswd vncpasswd kasmvncconfig vncconfig kasmxproxy; do \
      rm -f "/usr/bin/$command"; \
      printf '#!/bin/sh\nexec /usr/bin/env -u LD_LIBRARY_PATH -u LD_PRELOAD %s/bin/%s "$@"\n' "$PACKAGE" "$command" > "/usr/bin/$command"; \
      chmod 755 "/usr/bin/$command"; \
    done && \
    cp -r "$PACKAGE/share/kasmvnc/www/." /usr/share/kasmvnc/www/ && \
    mkdir -p /run/opengl-driver/lib && chown -R 1000:0 /run/opengl-driver && \
    chmod 755 /usr/local/bin/nix-core-entrypoint
USER 1000
ENTRYPOINT ["/usr/local/bin/nix-core-entrypoint"]
CMD ["--wait"]
