import analysis.cumulative_transience as ct
import json
import os

file_path = 'analysis/01 sustained bass [2025-12-29-22-19-46].wav'
result = ct.analyze_audio(file_path)
if result:
    all_data = {os.path.basename(file_path): result}
    with open('analysis/analyze.py', 'r') as f:
        content = f.read()
        start_marker = 'html_template = """'
        end_marker = '"""'
        start_idx = content.find(start_marker) + len(start_marker)
        end_idx = content.find(end_marker, start_idx)
        template = content[start_idx:end_idx]
    report = template.replace("DATA_PLACEHOLDER", json.dumps(all_data))
    with open('analysis/test_report.html', 'w') as f:
        f.write(report)
