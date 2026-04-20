from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import os
import re
import sys
import xml.etree.ElementTree as et

from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence


PLATFORM_ALIASES = {
    "win32": "windows_x64",
    "cygwin": "windows_x64",
    "linux": "linux_x86_64",
}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def normalize_name(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def load_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def dump_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)


def detect_host_platform() -> str:
    if sys.platform.startswith("linux"):
        machine = os.uname().machine.lower()
        if machine in {"aarch64", "arm64"}:
            return "linux_arm64"
        return "linux_x86_64"
    return PLATFORM_ALIASES.get(sys.platform, sys.platform)


def repo_root_from(path: Path) -> Path:
    return path.resolve().parents[2]


def read_text_if_exists(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


def find_readme(example_dir: Path) -> Path | None:
    for candidate in (example_dir / "README.md", example_dir / "readme.md"):
        if candidate.exists():
            return candidate
    return None


def parse_examples_index(index_path: Path) -> Dict[str, Dict[str, Any]]:
    mapping: Dict[str, Dict[str, Any]] = {}
    if not index_path.exists():
        return mapping

    line_re = re.compile(r"^\|\s*\[(?P<label>[^\]]+)\]\((?P<link>[^)]+)\)\s*\|\s*`(?P<target>[^`]+)`\s*\|\s*(?P<requires>[^|]+)\|\s*(?P<desc>.+?)\|\s*$")
    for line in index_path.read_text(encoding="utf-8").splitlines():
        match = line_re.match(line)
        if not match:
            continue
        link = match.group("link").strip()
        directory = link.rsplit("/", 1)[0]
        mapping[directory] = {
            "target": match.group("target").strip(),
            "requires": match.group("requires").strip(),
            "description": match.group("desc").strip(),
        }
    return mapping


def extract_section(text: str, header: str) -> List[str]:
    lines = text.splitlines()
    in_section = False
    collected: List[str] = []
    for line in lines:
        if line.strip().lower() == header.lower():
            in_section = True
            continue
        if in_section and line.startswith("## "):
            break
        if in_section:
            collected.append(line)
    return collected


def parse_supported_devices(readme_text: str) -> Dict[str, List[str]]:
    section = extract_section(readme_text, "## Supported Devices")
    series: List[str] = []
    models: List[str] = []
    for line in section:
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        if "Device Series" in stripped or stripped.startswith("|---") or stripped.startswith("|---"):
            continue
        parts = [item.strip() for item in stripped.strip("|").split("|")]
        if len(parts) < 2:
            continue
        series_name = parts[0]
        model_list = [item.strip() for item in parts[1].split(",") if item.strip()]
        if series_name:
            series.append(series_name)
        models.extend(model_list)
    return {
        "series": series,
        "models": models,
    }


def parse_target_from_cmake(cmake_path: Path) -> str:
    text = cmake_path.read_text(encoding="utf-8")
    project_match = re.search(r"project\(([^)\s]+)", text)
    if project_match:
        return project_match.group(1).strip()
    raise ValueError(f"Failed to parse target from {cmake_path}")


def is_leaf_example_cmake(cmake_path: Path) -> bool:
    text = cmake_path.read_text(encoding="utf-8")
    return "add_executable(" in text or "project(" in text


def normalize_requires(value: str) -> List[str]:
    lowered = value.lower().strip()
    if not lowered or lowered == "none":
        return []
    return [normalize_name(item) for item in lowered.split(",") if item.strip()]


def infer_platforms(case_id: str, readme_text: str) -> List[str]:
    lowered = readme_text.lower()
    if "linux / gmsl" in lowered or "gmsl" in case_id:
        return ["linux_x86_64", "linux_arm64"]
    return ["windows_x64", "linux_x86_64", "linux_arm64"]


def infer_case_behavior(case_id: str, requires: Sequence[str], readme_text: str) -> Dict[str, Any]:
    lowered = readme_text.lower()
    supports_png_save = "png" in lowered and "save" in lowered
    required_capabilities: List[str] = []
    auto_keys: List[str] = []
    stdin_lines: List[str] = []
    timeout_sec = 20
    min_device_count = 1
    allow_destructive = False

    if supports_png_save:
        auto_keys = ["S", "ESC"]
        timeout_sec = 25
    elif "opencv" in requires or case_id.startswith("beginner/") or case_id.startswith("advanced/") or case_id.startswith("lidar_examples/"):
        auto_keys = ["ESC"]

    if case_id.startswith("lidar_examples/"):
        required_capabilities.append("lidar")

    if "at least two supported devices" in lowered or "multi_devices_sync" in case_id:
        required_capabilities.append("multi_device")
        min_device_count = 2

    if "gmsltrigger" in case_id:
        required_capabilities.append("gmsl")

    if case_id == "advanced/forceip":
        required_capabilities.extend(["network_device", "destructive"])
        allow_destructive = True

    if case_id == "advanced/optional_depth_presets_update":
        required_capabilities.extend(["depth_preset_update", "destructive"])
        allow_destructive = True

    if case_id == "advanced/post_processing":
        stdin_lines = ["q"]

    if case_id == "advanced/preset":
        auto_keys = ["ESC"]

    if case_id == "lidar_examples/4.lidar_playback":
        stdin_lines = ["${DEFAULT_BAG}"]
        auto_keys = ["ESC"]
        required_capabilities.append("playback_bag")
        timeout_sec = 25

    if case_id == "lidar_examples/3.lidar_record":
        stdin_lines = ["${CASE_OUTPUT_DIR}/lidar_record.bag"]
        auto_keys = ["ESC"]
        timeout_sec = 25

    return {
        "supports_png_save": supports_png_save,
        "required_capabilities": sorted(set(required_capabilities)),
        "auto_keys": auto_keys,
        "auto_key_initial_delay_ms": 1800,
        "auto_key_interval_ms": 1200,
        "stdin_lines": stdin_lines,
        "timeout_sec": timeout_sec,
        "min_device_count": min_device_count,
        "allow_destructive": allow_destructive,
    }


def apply_override(case: Dict[str, Any], override: Dict[str, Any]) -> Dict[str, Any]:
    merged = dict(case)
    for key, value in override.items():
        if key in {"required_capabilities"}:
            merged[key] = sorted(set(merged.get(key, []) + list(value)))
        else:
            merged[key] = value
    return merged


def discover_cases(workspace_root: Path, overrides_path: Path | None = None) -> Dict[str, Any]:
    examples_root = workspace_root / "examples"
    index_info = parse_examples_index(examples_root / "README.md")
    overrides = load_json(overrides_path, {}) if overrides_path else {}
    cases: List[Dict[str, Any]] = []

    for cmake_path in sorted(examples_root.rglob("CMakeLists.txt")):
        if cmake_path == examples_root / "CMakeLists.txt":
            continue
        if not is_leaf_example_cmake(cmake_path):
            continue
        example_dir = cmake_path.parent
        relative_dir = example_dir.relative_to(examples_root).as_posix()
        if relative_dir == "utils":
            continue
        if relative_dir.startswith("application/"):
            continue
        if "publish_files" in relative_dir:
            continue

        readme_path = find_readme(example_dir)
        readme_text = read_text_if_exists(readme_path) if readme_path else ""
        index_record = index_info.get(relative_dir, {})
        target = index_record.get("target") or parse_target_from_cmake(cmake_path)
        requires = normalize_requires(index_record.get("requires", ""))
        devices = parse_supported_devices(readme_text)
        inferred = infer_case_behavior(relative_dir, requires, readme_text)

        case = {
            "id": relative_dir,
            "name": relative_dir.split("/")[-1],
            "category": relative_dir.split("/")[0],
            "directory": (Path("examples") / relative_dir).as_posix(),
            "readme": readme_path.relative_to(workspace_root).as_posix() if readme_path else "",
            "target": target,
            "description": index_record.get("description", ""),
            "requires": requires,
            "supported_platforms": infer_platforms(relative_dir, readme_text),
            "supported_series": devices["series"],
            "supported_models": devices["models"],
            **inferred,
        }

        if relative_dir in overrides:
            case = apply_override(case, overrides[relative_dir])
        cases.append(case)

    return {
        "generated_at": utc_now(),
        "workspace_root": str(workspace_root),
        "case_count": len(cases),
        "cases": cases,
    }


def supports_pool(case: Dict[str, Any], pool: Dict[str, Any]) -> bool:
    if pool["platform"] not in case["supported_platforms"]:
        return False
    if case.get("min_device_count", 1) > pool.get("device_count", 1):
        return False
    if case.get("allow_destructive") and not pool.get("allow_destructive", False):
        return False

    pool_caps = set(pool.get("capabilities", []))
    required_caps = set(case.get("required_capabilities", []))
    if not required_caps.issubset(pool_caps):
        return False

    supported_models = {normalize_name(item) for item in case.get("supported_models", [])}
    supported_series = {normalize_name(item) for item in case.get("supported_series", [])}
    pool_models = {normalize_name(item) for item in pool.get("device_models", [])}
    pool_series = {normalize_name(item) for item in pool.get("device_series", [])}

    if not supported_models and not supported_series:
        return True
    if supported_models & pool_models:
        return True
    if supported_series & pool_series:
        return True
    return False


def pool_match_rank(case: Dict[str, Any], pool: Dict[str, Any]) -> tuple[int, int, int, int]:
    """Return ranking for candidate pool selection.

    Higher tuple value means a better candidate. Ranking priorities:
    1) Exact model overlap preferred over series overlap.
    2) Narrower pool model/series scope preferred (more specific pool wins).
    3) Lower configured priority value preferred as tie-breaker.
    """
    supported_models = {normalize_name(item) for item in case.get("supported_models", [])}
    supported_series = {normalize_name(item) for item in case.get("supported_series", [])}
    pool_models = {normalize_name(item) for item in pool.get("device_models", [])}
    pool_series = {normalize_name(item) for item in pool.get("device_series", [])}

    has_model_overlap = bool(supported_models & pool_models)
    has_series_overlap = bool(supported_series & pool_series)
    match_level = 2 if has_model_overlap else (1 if has_series_overlap else 0)

    # Prefer narrower pools when multiple pools match the same case.
    model_specificity = -len(pool_models)
    series_specificity = -len(pool_series)
    priority_tiebreak = -int(pool.get("priority", 100))
    return (match_level, model_specificity, series_specificity, priority_tiebreak)


def batch(items: Sequence[str], batch_size: int) -> List[List[str]]:
    result: List[List[str]] = []
    for index in range(0, len(items), batch_size):
        result.append(list(items[index:index + batch_size]))
    return result


def generate_matrix(manifest: Dict[str, Any], runner_pools: Dict[str, Any], batch_size: int = 6) -> Dict[str, Any]:
    pools = sorted(runner_pools.get("pools", []), key=lambda item: item.get("priority", 100))
    grouped: Dict[str, Dict[str, Any]] = {}
    unassigned: List[Dict[str, Any]] = []

    platforms = sorted({pool["platform"] for pool in pools})

    for case in manifest.get("cases", []):
        assigned_platforms: List[str] = []
        for platform in platforms:
            candidates = [pool for pool in pools if pool["platform"] == platform and supports_pool(case, pool)]
            if not candidates:
                continue
            pool = max(candidates, key=lambda item: pool_match_rank(case, item))
            assigned_platforms.append(platform)
            grouped.setdefault(pool["name"], {"pool": pool, "case_ids": []})["case_ids"].append(case["id"])

        missing_platforms = [platform for platform in case["supported_platforms"] if platform not in assigned_platforms]
        if missing_platforms:
            unassigned.append({
                "case_id": case["id"],
                "missing_platforms": missing_platforms,
            })

    matrix: List[Dict[str, Any]] = []
    for pool_name, payload in sorted(grouped.items()):
        pool = payload["pool"]
        case_ids = sorted(payload["case_ids"])
        case_map = {case["id"]: case for case in manifest.get("cases", [])}
        for batch_index, case_chunk in enumerate(batch(case_ids, batch_size), start=1):
            targets = sorted({case_map[case_id]["target"] for case_id in case_chunk})
            matrix.append({
                "job_id": f"{pool_name}-{batch_index}",
                "display_name": f"{pool_name} batch {batch_index}",
                "pool_name": pool_name,
                "platform": pool["platform"],
                "runs_on": json.dumps(pool["runs_on"]),
                "case_ids": ";".join(case_chunk),
                "targets": ";".join(targets),
            })

    return {
        "generated_at": utc_now(),
        "matrix": matrix,
        "unassigned": unassigned,
    }


def resolve_binary(build_root: Path, target: str) -> Path | None:
    names = [target]
    if os.name == "nt":
        names.append(f"{target}.exe")

    candidates: List[Path] = []
    for name in names:
        candidates.extend(path for path in build_root.rglob(name) if path.is_file())

    if not candidates:
        return None

    def score(path: Path) -> tuple[int, int, int]:
        parts = {part.lower() for part in path.parts}
        in_bin = 0 if "bin" in parts else 1
        in_install = 1 if "install" in parts else 0
        return (in_install, in_bin, len(path.parts))

    return sorted(candidates, key=score)[0]


def resolve_example_workdir(build_root: Path, binary_path: Path) -> Path:
    for path in build_root.rglob("extensions"):
        if path.is_dir():
            return path.parent
    return binary_path.parent


def default_bag_path(workspace_root: Path) -> str:
    rosbag_root = workspace_root / "tests" / "resource" / "rosbag"
    if not rosbag_root.exists():
        return ""
    for bag_path in sorted(rosbag_root.glob("*.bag")):
        return str(bag_path)
    return ""


def default_preset_bin(workspace_root: Path) -> str:
    for folder in [workspace_root / "tests" / "resource" / "present", workspace_root / "tests" / "resource" / "preset"]:
        if folder.exists():
            for bin_path in sorted(folder.glob("*.bin")):
                return str(bin_path)
    return ""


def substitute_tokens(value: str, context: Dict[str, str]) -> str:
    result = value
    for key, token_value in context.items():
        result = result.replace(f"${{{key}}}", token_value)
    return result


def case_dir_name(case_id: str) -> str:
    return case_id.replace("/", "__")


def summarize_results(results: Sequence[Dict[str, Any]]) -> Dict[str, int]:
    total = len(results)
    passed = sum(1 for item in results if item["status"] == "passed")
    failed = sum(1 for item in results if item["status"] == "failed")
    skipped = sum(1 for item in results if item["status"] == "skipped")
    return {
        "total": total,
        "passed": passed,
        "failed": failed,
        "skipped": skipped,
    }


def write_junit_report(results: Sequence[Dict[str, Any]], output_path: Path, suite_name: str) -> None:
    suite = et.Element("testsuite", name=suite_name)
    stats = summarize_results(results)
    suite.set("tests", str(stats["total"]))
    suite.set("failures", str(stats["failed"]))
    suite.set("skipped", str(stats["skipped"]))

    for result in results:
        testcase = et.SubElement(
            suite,
            "testcase",
            classname=result.get("suite_name", suite_name),
            name=result["case_id"],
            time=f"{result.get('duration_sec', 0.0):.3f}",
        )
        if result["status"] == "failed":
            failure = et.SubElement(testcase, "failure", message=result.get("message", "failed"))
            failure.text = result.get("message", "failed")
        elif result["status"] == "skipped":
            skipped = et.SubElement(testcase, "skipped", message=result.get("message", "skipped"))
            skipped.text = result.get("message", "skipped")

    tree = et.ElementTree(suite)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tree.write(output_path, encoding="utf-8", xml_declaration=True)


def render_html_report(results: Sequence[Dict[str, Any]], output_path: Path, title: str) -> None:
    rows: List[str] = []
    output_path.parent.mkdir(parents=True, exist_ok=True)

    for result in results:
        color = {
            "passed": "#1f8b4c",
            "failed": "#b42318",
            "skipped": "#b54708",
        }.get(result["status"], "#667085")
        images_html = ""
        if result.get("saved_images"):
            parts = []
            for image_path in result["saved_images"]:
                image_ref = Path(image_path)
                if image_ref.is_absolute():
                    rel_path = os.path.relpath(image_path, output_path.parent).replace("\\", "/")
                else:
                    rel_path = image_ref.as_posix()
                parts.append(f'<a href="{html.escape(rel_path)}"><img src="{html.escape(rel_path)}" alt="{html.escape(Path(image_path).name)}"></a>')
            images_html = "".join(parts)

        log_html = "-"
        if result.get("log_path"):
            log_ref = Path(result["log_path"])
            if log_ref.is_absolute():
                rel_log = os.path.relpath(result["log_path"], output_path.parent).replace("\\", "/")
            else:
                rel_log = log_ref.as_posix()
            log_html = f'<a href="{html.escape(rel_log)}">log</a>'

        rows.append(
            "<tr>"
            f"<td>{html.escape(result.get('suite_name', '-'))}</td>"
            f"<td>{html.escape(result['case_id'])}</td>"
            f"<td>{html.escape(result.get('target', '-'))}</td>"
            f"<td>{html.escape(result.get('platform', '-'))}</td>"
            f"<td>{html.escape(result.get('pool_name', '-'))}</td>"
            f"<td><span class=\"status\" style=\"background:{color};\">{html.escape(result['status'])}</span></td>"
            f"<td>{result.get('duration_sec', 0.0):.2f}s</td>"
            f"<td>{html.escape(result.get('message', '-'))}</td>"
            f"<td>{log_html}</td>"
            f"<td class=\"images\">{images_html or '-'}</td>"
            "</tr>"
        )

    stats = summarize_results(results)
    document = f"""<!DOCTYPE html>
<html lang=\"zh-CN\">
<head>
  <meta charset=\"utf-8\">
  <title>{html.escape(title)}</title>
  <style>
    body {{ font-family: "Segoe UI", "PingFang SC", sans-serif; background: #f8fafc; color: #101828; margin: 24px; }}
    h1 {{ margin-bottom: 8px; }}
    .summary {{ display: flex; gap: 12px; margin: 0 0 20px; flex-wrap: wrap; }}
    .card {{ background: white; border: 1px solid #d0d5dd; border-radius: 12px; padding: 12px 16px; min-width: 120px; }}
    table {{ width: 100%; border-collapse: collapse; background: white; border-radius: 12px; overflow: hidden; }}
    th, td {{ border: 1px solid #eaecf0; padding: 10px 12px; vertical-align: top; text-align: left; }}
    th {{ background: #f2f4f7; }}
    .status {{ color: white; border-radius: 999px; padding: 2px 10px; font-size: 12px; display: inline-block; }}
    .images img {{ width: 120px; height: 90px; object-fit: cover; border: 1px solid #d0d5dd; border-radius: 8px; margin-right: 8px; margin-bottom: 8px; background: #fff; }}
    a {{ color: #175cd3; text-decoration: none; }}
  </style>
</head>
<body>
  <h1>{html.escape(title)}</h1>
  <div>Generated at {html.escape(utc_now())}</div>
  <div class=\"summary\">
    <div class=\"card\"><strong>Total</strong><div>{stats['total']}</div></div>
    <div class=\"card\"><strong>Passed</strong><div>{stats['passed']}</div></div>
    <div class=\"card\"><strong>Failed</strong><div>{stats['failed']}</div></div>
    <div class=\"card\"><strong>Skipped</strong><div>{stats['skipped']}</div></div>
  </div>
  <table>
    <thead>
      <tr>
        <th>Suite</th>
        <th>Case</th>
        <th>Target</th>
        <th>Platform</th>
        <th>Pool</th>
        <th>Status</th>
        <th>Duration</th>
        <th>Message</th>
        <th>Log</th>
        <th>Images</th>
      </tr>
    </thead>
    <tbody>
      {''.join(rows)}
    </tbody>
  </table>
</body>
</html>
"""
    output_path.write_text(document, encoding="utf-8")


def manifest_argument_parser(description: str) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--workspace-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--overrides", default="")
    return parser
