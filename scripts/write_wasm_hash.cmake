# 把 wasm 的 SHA256 写成一行文本（heditor.hash）：生产环境用它当 assets.bust，
#   同一份构建命中缓存、换了构建自然失效。时间戳做不到这一点。
file(SHA256 "${WASM}" hash)
file(WRITE "${OUT}" "${hash}\n")
