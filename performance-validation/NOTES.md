# Notes

## DTLMod source patch

During this study a genuine DTLMod bug was found and fixed: a null-pointer
dereference crash when a File stream is opened by multiple publishers *and*
multiple subscribers (the previously untested M x N configuration). The fix
makes engine creation and actor registration a single critical section in
`Stream::open`. See REPORT.md section 7.

The fix is provided here as `dtlmod-stream-open-fix.patch`. Apply it from a
DTLMod checkout with:

    git apply dtlmod-stream-open-fix.patch

It could not be pushed to `simgrid/DTLMod` from this environment (no write
access; the target branch does not exist upstream), so it is preserved here in
the ADIOS2 fork alongside the rest of the study.
