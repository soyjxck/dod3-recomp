"""The frames the game writes (PS3RECOMP_FRAME_GRAB, RSX_REPLAY_DUMP_PATH): binary
PPM, P6. read_ppm(path) -> (width, height, pixels), pixels the RGB bytes."""


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    tok, i = [], 0
    while len(tok) < 4:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        tok.append(d[i:j])
        i = j
    i += 1  # the one whitespace byte after the maximum value
    if tok[0] != b"P6":
        raise ValueError(f"{path}: not a P6 PPM")
    w, h = int(tok[1]), int(tok[2])
    return w, h, d[i:i + w * h * 3]
