# release/

Built packages land here; nothing in this folder is committed (see `.gitignore`).

    sh build/package_sf2000.sh v026
    cp out/sf2000/BobrHopper-SF2000-v026.zip release/

Attach the zip from here to a GitHub release. Keeping it out of git is deliberate: a 10 MB zip per version would
make the repository heavier than the game.
