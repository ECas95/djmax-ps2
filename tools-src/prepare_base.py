"""Convert only Fever's original movies from a user-provided PSP_GAME directory."""
import argparse
from pathlib import Path
from pmf_convert import main

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('psp_game', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    main(str(args.psp_game / 'USRDIR/movie'), str(args.output / 'PS2_DATA/movie'),
         ['opening', 'ending', 'ons', 'soundstorm'], None)
