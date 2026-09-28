# NID name lookup

`nids.inc` remains the canonical HLE export registry. `known_nids.inc` adds
name lookup for functions and variables in the merged Vita database without
declaring HLE bridges or allocating guest variables. Both native and browser
builds compile the same `src/nids.cpp`.

The imported database is
[merged-vita-nid-db.yml](https://raw.githubusercontent.com/computerman00/BinaryNinja-PSVitaLoader/refs/heads/main/merged-vita-nid-db.yml),
SHA-256 `cf3c3b8e1566067848cf728231061fa4dcd115193ad97cc471b2c8bd551c7976`.
It contains 9,270 function/variable records representing 9,183 distinct NIDs.
Of these, 2,150 were absent from the canonical registry. All are now recognized;
the union with the repository's existing registry contains 9,519 distinct NIDs.

`import_name(nid)` preserves existing canonical spellings. When a missing NID
appears more than once, the first source spelling becomes its default name.
`import_name(nid, library_nid)` selects a library-specific spelling where it
differs, using the 395 entries in `nid_aliases.inc`, and otherwise falls back
to `import_name(nid)`. Unknown NIDs still return `"UNRECOGNISED"`.

Three upstream records conflict even after qualifying by library. The existing
canonical spelling is retained, and the generator records both spellings in
comments in `nid_aliases.inc`:

| Library | NID | Retained name | Conflicting source name |
| --- | --- | --- | --- |
| SceLibMonoBridge | `0x58ABAD62` | `pss_errno_loc` | `pss_app_exit_liveboard` |
| SceLibMtp | `0xE3727775` | `sceMtpBeginGetSystemSetting` | `sceMtpBeginHttpGetPropertyWithUrl` |
| ScePafResource | `0xABFA169F` | `scePafResourceGetAttributeIdInt` | `scePafResourceGetAttributeIdIntLpb` |

Name recognition does not imply an API is implemented. The name-only catalog
does not participate in `resolve_import`, `has_hle_implementation`, or browser
HLE export selection.

## Regenerate and verify

Download the source database to a local file, then run:

```sh
node vita3k/nids/generate_known_nids.mjs /tmp/merged-vita-nid-db.yml
node vita3k/nids/generate_known_nids.mjs /tmp/merged-vita-nid-db.yml --check
node vita3k/nids/tests/generate_known_nids.test.mjs
```

The generator accepts the database's unquoted version-2 YAML subset and rejects
unrecognized syntax rather than silently dropping records. It includes the
input SHA-256 in both generated files. `--check` fails if the checked-in output
does not exactly match regeneration against the source and current HLE registry.
Regenerate after promoting a name-only NID into `nids.inc` to avoid duplicate
switch cases.

The standalone native test exercises the production lookup functions for all
9,519 names, all 395 library aliases, canonical conflict handling, and unknown
NIDs. It requires only a C++17 compiler:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Ivita3k/nids/include \
  vita3k/nids/src/nids.cpp vita3k/nids/tests/nids_lookup_tests.cpp \
  -o /tmp/vita-nids-lookup-tests
/tmp/vita-nids-lookup-tests
```

Native CMake builds also expose this test as the `nids-tests` target and `nids`
CTest test.
