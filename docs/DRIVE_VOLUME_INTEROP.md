# Filesystem-name interoperability

MS-FSCC 2.5.1 defines FileSystemName as a length-delimited Unicode informational field. The installed independent FreeRDP client includes zero code units at its end; this was observed as the sole failure in CI run 37978505694. LRDP consumes exactly FileSystemNameLength bytes and permits only an all-zero UTF-16 suffix. The resulting name must remain nonempty, all nonzero code units are retained, and embedded NULs, odd lengths, malformed surrogate pairs, and over-limit lengths are rejected. The name never selects filesystem behavior and this compatibility exception does not apply to paths or authenticated principals.

Primary source: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/ebc7e6e5-4650-4e54-b17c-cf60f6fbeeaa
