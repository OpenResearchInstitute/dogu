# yocto

Yocto / OpenEmbedded recipes for packaging dōgu components into
PetaLinux rootfs images.

**Status: design phase.** Not yet populated.

## Planned layout

```
yocto/
└── meta-dogu/
    ├── conf/
    │   └── layer.conf
    ├── recipes-oriinit/
    │   └── liboriinit/
    │       └── liboriinit_git.bb
    ├── recipes-tools/
    │   └── dma_listen/
    │       └── dma_listen_git.bb
    └── recipes-services/
        ├── hatsuon/
        ├── dvbs2-mon/
        └── kabura-mon/
```

## Integration with Haifuraiya / PetaLinux 2022.2

Adds `meta-dogu` as a Yocto layer in the project's `bblayers.conf`,
then includes `dogu-image` (or individual packages) in the image's
`IMAGE_INSTALL`. Each recipe pulls its source from this git repo
at a specified commit, allowing per-release pinning of ARM-side
software versions.

## See also

- Mode-Dynamic-Transponder repo for the parent PetaLinux build
  configuration
