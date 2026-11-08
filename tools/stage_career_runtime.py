"""Stage the pinned Windows Unicorn runtime and matching source during build."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import shutil
import tempfile
from urllib.request import urlopen
from zipfile import ZipFile


ROOT = Path(__file__).resolve().parents[1]
WHEEL = ("70/3d/de7be9bd1addabe6d8a1369381f8a080400c349850e978689c5e18287957/unicorn-2.1.4-cp37-abi3-win_amd64.whl",
         "d7107500c64ce5c168fbff6bef9485b5db1350050036f4cea568650cf8bdbdf5")
SOURCE = ("b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/unicorn-2.1.4.tar.gz",
          "00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff")


def distribution(path, digest):
    cache = ROOT / ".deps" / "career-unicorn"
    cache.mkdir(exist_ok=True, parents=True)
    destination = cache / Path(path).name
    if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest() == digest:
        return destination
    with tempfile.TemporaryDirectory(dir=cache) as folder:
        download = Path(folder) / destination.name
        with urlopen("https://files.pythonhosted.org/packages/" + path, timeout=60) as response:
            with download.open("xb") as output:
                shutil.copyfileobj(response, output)
        if hashlib.sha256(download.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Unexpected digest for {destination.name}")
        download.replace(destination)
    return destination


def stage(runtime, notices=None):
    with ZipFile(distribution(*WHEEL)) as wheel:
        for info in wheel.infolist():
            path = PurePosixPath(info.filename)
            if (path.parts[0] != "unicorn" or ".." in path.parts
                    or (path.suffix != ".py" and path.as_posix() != "unicorn/lib/unicorn.dll")):
                continue
            target = runtime / path
            target.parent.mkdir(exist_ok=True, parents=True)
            data = wheel.read(info)
            if target.is_file() and target.read_bytes() == data:
                continue
            with tempfile.TemporaryDirectory(dir=target.parent) as temp:
                staged = Path(temp) / target.name
                staged.write_bytes(data)
                staged.replace(target)
    if not (runtime / "unicorn/lib/unicorn.dll").is_file():
        raise ValueError("Missing career codec runtime library")
    if notices is not None:
        notices.mkdir(exist_ok=True, parents=True)
        source = distribution(*SOURCE)
        shutil.copyfile(source, notices / source.name)
        (notices / "README.txt").write_text(
            "Unicorn Engine 2.1.4 (GPLv2) and its Python bindings.\n"
            "The accompanying source distribution includes build files and license notices.\n"
            "https://github.com/unicorn-engine/unicorn/tree/2.1.4\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    args = parser.parse_args()
    stage(args.runtime)
