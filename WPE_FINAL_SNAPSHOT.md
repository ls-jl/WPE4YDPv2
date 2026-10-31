# Final WPE Snapshot

This branch preserves the final WPE implementation before the Chromium
migration. It is a source archive, not a newly verified public binary release.

- Original source commit: `e0efd6638eef0fa9a270161b36d61e8935096356`.
- Last packaged runtime commit: `33e1537`.
- Source and existing runtime resources are unchanged from that tree, except
  for this note and omission of the two new LFS artifacts listed below.
- Chromium development and intermediate private GPU packages are not included.
- The original local history and the remote `dev` branch remain unchanged.

## Binary Upload Blocked

GitHub rejected the LFS upload because the repository exceeded its LFS budget.
The following artifacts are retained locally but deliberately omitted from
this source snapshot, rather than publishing unresolved new LFS pointers:

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| `8001779591038449.1_0_0.amr` | 172135904 | `b3ce392beac43480dba0ada9ac01289a692a2dfb57c71905f9c588bd889b2cf1` |
| `assets/wpe-runtime/lib/libWPEWebKit-2.0.so.1` | 160069592 | `c21f52147ec9ae082da3fa719e8f1cd5e5646eec855c01d04db1f4698de98811` |

To inspect sources while the LFS quota is blocked, skip automatic LFS downloads:

```sh
GIT_LFS_SKIP_SMUDGE=1 git clone --branch wpe-final-source https://github.com/ls-jl/WPE4YDPv2.git
```

Existing LFS dependencies inherited from the remote baseline still require
LFS access or a verified local copy. Skipping downloads is for source inspection;
it does not produce a complete runnable runtime or satisfy packaging checks.

The final AMR was hash-verified, contained one WebKit ELF, and passed the
private Mali/KO/disputed-plugin archive scan. This is not a blanket license
or hardware acceptance statement. Preserve upstream notices and use the
pinned source, patch series and overlay/build manifests when rebuilding.
See `THIRD_PARTY_LICENSES.md`, `wpe-drm/webkit-patches/` and
`tools/GPU_RENDER_CLEANUP_STATUS.md` for implementation and acceptance limits.

Packaging needs a rebuilt WebKit library. Existing README packaging commands
are not runnable from this source-only snapshot without rebuilding it.
Do not substitute an unrelated old WebKit binary or private GPU package.
