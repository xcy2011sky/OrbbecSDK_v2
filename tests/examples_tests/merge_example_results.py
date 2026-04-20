from __future__ import annotations

import argparse
import shutil

from pathlib import Path

from examples_test_utils import case_dir_name, dump_json, load_json, render_html_report, summarize_results, write_junit_report


def copy_case_assets(result: dict, suite_dir: Path, output_dir: Path) -> dict:
    merged_result = dict(result)
    case_dir = suite_dir / "cases" / case_dir_name(result["case_id"])
    asset_dir = output_dir / "assets" / merged_result.get("suite_name", suite_dir.name) / case_dir_name(result["case_id"])

    if case_dir.exists():
        asset_dir.mkdir(parents=True, exist_ok=True)

        log_src = case_dir / "run.log"
        if log_src.exists():
            log_dst = asset_dir / "run.log"
            shutil.copy2(log_src, log_dst)
            merged_result["log_path"] = log_dst.relative_to(output_dir).as_posix()
        else:
            merged_result["log_path"] = ""

        copied_images = []
        image_names = [Path(path).name for path in result.get("saved_images", [])]
        if not image_names:
            image_names = [path.name for path in sorted(case_dir.glob("*.png"))]

        for image_name in image_names:
            image_src = case_dir / image_name
            if not image_src.exists():
                continue
            image_dst = asset_dir / image_name
            shutil.copy2(image_src, image_dst)
            copied_images.append(image_dst.relative_to(output_dir).as_posix())
        merged_result["saved_images"] = copied_images
    else:
        merged_result["log_path"] = ""
        merged_result["saved_images"] = []

    return merged_result


def main() -> int:
    parser = argparse.ArgumentParser(description="Merge examples suite results into aggregate reports")
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--title", default="Examples Aggregate Report")
    args = parser.parse_args()

    input_dir = Path(args.input_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    merged_results = []
    for result_file in sorted(input_dir.rglob("results.json")):
        payload = load_json(result_file, {})
        suite_dir = result_file.parent
        for result in payload.get("results", []):
            merged_results.append(copy_case_assets(result, suite_dir, output_dir))

    dump_json(output_dir / "results.json", {
        "stats": summarize_results(merged_results),
        "results": merged_results,
    })
    write_junit_report(merged_results, output_dir / "junit.xml", "examples-aggregate")
    render_html_report(merged_results, output_dir / "report.html", args.title)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
