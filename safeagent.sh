#!/usr/bin/env bash
bwrap \
  --ro-bind /usr /usr \
  --symlink usr/bin /bin \
  --symlink usr/lib /lib \
  --symlink usr/lib64 /lib64 \
  --ro-bind /etc/ssl /etc/ssl \
  --ro-bind /etc/resolv.conf /etc/resolv.conf \
  --bind "$HOME/.config/opencode" "$HOME/.config/opencode" \
  --bind "$HOME/.local/share/opencode" "$HOME/.local/share/opencode" \
  --ro-bind /home/akyirr/.local/bin /home/akyirr/.local/bin \
  --bind /home/akyirr/Programs/Flutter /home/akyirr/Programs/Flutter \
  --bind /home/akyirr/Programs/Android /home/akyirr/Programs/Android \
  --bind "$HOME/.android" "$HOME/.android" \
  --bind /home/akyirr/.platformio /home/akyirr/.platformio \
  --bind "$HOME/.pub-cache" "$HOME/.pub-cache" \
  --bind "$PWD" "$PWD" \
  --tmpfs /tmp \
  --share-net \
  --die-with-parent \
  --dev /dev \
  --proc /proc \
  --setenv ANDROID_HOME "/home/akyirr/Programs/Android" \
  --setenv JAVA_HOME "/usr/lib/jvm/java-26-openjdk" \
  /usr/bin/opencode --continue --agent --auto
