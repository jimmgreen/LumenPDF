# LumenPDF 在线升级签名工具（ECDSA P-256 / SHA-256，签名为 64 字节 r||s 的 Base64）。
#
#   python scripts/update_signing.py keygen                 生成私钥（仓库外）并写入 app/update_public_key.h
#   python scripts/update_signing.py pubkey                 用现有私钥重新生成 app/update_public_key.h
#   python scripts/update_signing.py sign <file>            写出 <file>.sig
#   python scripts/update_signing.py verify <file>          用 app/update_public_key.h 中的公钥校验 <file>.sig
#   python scripts/update_signing.py manifest --version 0.4.0 --notes-file notes.md \
#          --setup dist/LumenPDF-0.4.0-setup.exe --portable dist/LumenPDF-0.4.0-portable-x64.zip \
#          --out update/latest.json [--sums dist/SHA256SUMS]
#
# 私钥默认在 %USERPROFILE%\.lumenpdf\update-signing-key.pem（可用环境变量 LUMENPDF_SIGNING_KEY 指定）。
# 私钥绝不能进仓库；丢失后旧版本程序将无法验证新发布的升级，请离线备份。
# 依赖：pip install cryptography
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "app" / "update_public_key.h"
REPO = "jimmgreen/LumenPDF"
# 国内下载加速镜像（前缀 + GitHub 原始地址）。写进签名清单，镜像失效时只需发新清单即可更换。
MIRRORS = ["https://gh-proxy.com/", "https://ghfast.top/", "https://ghproxy.cxkpro.top/", "https://gh.zwy.one/", "https://ghproxy.net/"]


def key_path() -> Path:
    custom = os.environ.get("LUMENPDF_SIGNING_KEY")
    return Path(custom) if custom else Path.home() / ".lumenpdf" / "update-signing-key.pem"


def load_key() -> ec.EllipticCurvePrivateKey:
    path = key_path()
    if not path.exists():
        sys.exit(f"找不到签名私钥：{path}（先运行 keygen，或设置 LUMENPDF_SIGNING_KEY）")
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit("签名私钥必须是 ECDSA P-256")
    return key


def public_raw(key) -> bytes:
    pub = key.public_key() if hasattr(key, "public_key") else key
    point = pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return point[1:]  # 去掉 0x04 前缀，得到 X||Y（64 字节）


def write_header(key) -> None:
    raw = public_raw(key)
    fingerprint = hashlib.sha256(raw).hexdigest()[:16]
    rows = []
    for i in range(0, 64, 16):
        rows.append("    " + ",".join(f"0x{b:02x}" for b in raw[i:i + 16]) + ",")
    text = (
        "#pragma once\n"
        "// 由 scripts/update_signing.py 生成：在线升级清单签名公钥（ECDSA P-256，X||Y）。\n"
        f"// 指纹（SHA-256 前 16 位）：{fingerprint}\n"
        "#include <array>\n#include <cstdint>\n"
        "namespace lpdf::update {\n"
        "inline constexpr std::array<uint8_t,64> kUpdatePublicKey{\n" + "\n".join(rows) + "\n};\n}\n"
    )
    HEADER.write_text(text, encoding="utf-8", newline="\n")
    print(f"公钥已写入 {HEADER.relative_to(ROOT)}，指纹 {fingerprint}")


def header_public_key() -> ec.EllipticCurvePublicKey:
    text = HEADER.read_text(encoding="utf-8")
    body = text[text.index("{", text.index("kUpdatePublicKey")) + 1:]
    raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)[:64])
    return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + raw)


def sign_bytes(key, data: bytes) -> str:
    der = key.sign(data, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode("ascii")


def verify_bytes(pub, data: bytes, sig_b64: str) -> bool:
    raw = base64.b64decode(sig_b64.strip())
    if len(raw) != 64:
        return False
    der = encode_dss_signature(int.from_bytes(raw[:32], "big"), int.from_bytes(raw[32:], "big"))
    try:
        pub.verify(der, data, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_keygen(args) -> None:
    path = key_path()
    if path.exists() and not args.force:
        sys.exit(f"私钥已存在：{path}。更换密钥会让已发布的旧版本无法验证新升级；确需更换请加 --force。")
    key = ec.generate_private_key(ec.SECP256R1())
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                       serialization.NoEncryption()))
    print(f"私钥已生成：{path}（请立即离线备份，切勿提交到仓库）")
    write_header(key)


def cmd_sign(args) -> None:
    key = load_key()
    path = Path(args.file)
    sig = sign_bytes(key, path.read_bytes())
    Path(str(path) + ".sig").write_text(sig + "\n", encoding="ascii", newline="\n")
    if not verify_bytes(header_public_key(), path.read_bytes(), sig):
        sys.exit("签名与 app/update_public_key.h 中的公钥不匹配")
    print(f"已签名 {path}")


def cmd_verify(args) -> None:
    path = Path(args.file)
    ok = verify_bytes(header_public_key(), path.read_bytes(), Path(str(path) + ".sig").read_text())
    print("签名有效" if ok else "签名无效")
    sys.exit(0 if ok else 1)


def asset(path: Path, tag: str) -> dict:
    return {
        "name": path.name,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
        "urls": [f"https://github.com/{REPO}/releases/download/{tag}/{path.name}"],
    }


def cmd_manifest(args) -> None:
    key = load_key()
    tag = "v" + args.version
    notes = Path(args.notes_file).read_text(encoding="utf-8").strip() if args.notes_file else ""
    setup, portable = Path(args.setup), Path(args.portable)
    manifest = {
        "schema": 1,
        "product": "LumenPDF",
        "version": args.version,
        "tag": tag,
        "published": args.date or datetime.date.today().isoformat(),
        "page": f"https://github.com/{REPO}/releases/tag/{tag}",
        "notes": notes,
        "assets": {"setup": asset(setup, tag), "portable": asset(portable, tag)},
        "mirrors": MIRRORS,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    out.write_bytes(data)
    sig = sign_bytes(key, data)
    Path(str(out) + ".sig").write_text(sig + "\n", encoding="ascii", newline="\n")
    if not verify_bytes(header_public_key(), data, sig):
        sys.exit("签名与 app/update_public_key.h 中的公钥不匹配")
    print(f"清单已写入并签名：{out}")
    if args.sums:
        lines = [f"{manifest['assets'][k]['sha256']}  {manifest['assets'][k]['name']}" for k in ("setup", "portable")]
        lines.append(f"{hashlib.sha256(data).hexdigest()}  latest.json")
        Path(args.sums).write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
        print(f"校验和已写入：{args.sums}")


def main() -> None:
    parser = argparse.ArgumentParser(description="LumenPDF 在线升级签名工具")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("keygen"); p.add_argument("--force", action="store_true"); p.set_defaults(func=cmd_keygen)
    p = sub.add_parser("pubkey"); p.set_defaults(func=lambda a: write_header(load_key()))
    p = sub.add_parser("sign"); p.add_argument("file"); p.set_defaults(func=cmd_sign)
    p = sub.add_parser("verify"); p.add_argument("file"); p.set_defaults(func=cmd_verify)
    p = sub.add_parser("manifest")
    p.add_argument("--version", required=True)
    p.add_argument("--notes-file")
    p.add_argument("--setup", required=True)
    p.add_argument("--portable", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--sums")
    p.add_argument("--date")
    p.set_defaults(func=cmd_manifest)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
