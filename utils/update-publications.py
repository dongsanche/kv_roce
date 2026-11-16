# /// script
# requires-python = ">=3.9"
# dependencies = ["bibtexparser>=1.4,<2"]
# ///
"""Render publications.bib into both READMEs: uv run utils/update-publications.py."""

from pathlib import Path

import bibtexparser

ROOT = Path(__file__).resolve().parents[1]
START = "<!-- BEGIN PUBLICATIONS -->"
END = "<!-- END PUBLICATIONS -->"


def plain(value):
    return value.replace("{", "").replace("}", "")


def citation(entry):
    names = []
    authors = entry["author"].split(" and ")
    for author in authors[:3]:
        family, given = author.split(",", 1)
        initials = " ".join(word[0].upper() for word in given.split())
        names.append(f"{family.upper()} {initials}")
    if len(authors) > 3:
        names.append("et al.")
    author_text = ", ".join(names)
    title = plain(entry["title"])
    pages = entry["pages"].replace("--", "-")
    year = entry["year"]
    if entry["ENTRYTYPE"] == "article":
        publication = f"{entry['journal']}, {year}, {entry['volume']}: {pages}."
        text = f"{author_text} {title}[J]. {publication}"
    elif entry["ENTRYTYPE"] == "inproceedings":
        publication = (
            f"{entry['booktitle']}. {entry['address']}: {entry['publisher']}, {year}: {pages}."
        )
        text = f"{author_text} {title}[C]//{publication}"
    else:
        raise ValueError(f"Unsupported publication type: {entry['ENTRYTYPE']}")
    if "doi" in entry:
        text += f" DOI: [{entry['doi']}](https://doi.org/{entry['doi']})."
    else:
        text += f" [Paper]({entry['url']})."
    return text


def main():
    parser = bibtexparser.bparser.BibTexParser(common_strings=True)
    with (ROOT / "publications.bib").open(encoding="utf-8") as source:
        entries = bibtexparser.load(source, parser=parser).entries
    references = "\n\n".join(f"{i}. {citation(e)}" for i, e in enumerate(entries, 1))
    sections = {
        "README.md": (
            "## 引用本项目的论文\n\n"
            "以下论文引用了 ns-3-UB / OpenUSim。参考文献按 GB/T 7714 风格列出，"
            "完整书目信息见 [BibTeX 文件](publications.bib)。"
        ),
        "README_en.md": (
            "## Papers Citing This Project\n\n"
            "The following papers cite ns-3-UB / OpenUSim. References use GB/T 7714 style; "
            "full bibliographic records are available in [BibTeX](publications.bib)."
        ),
    }
    # Read and render both files before writing either one.
    rendered = {}
    for filename, intro in sections.items():
        path = ROOT / filename
        source = path.read_text(encoding="utf-8")
        before, rest = source.split(START)
        _, after = rest.split(END)
        rendered[path] = f"{before}{START}\n{intro}\n\n{references}\n{END}{after}"
    for path, content in rendered.items():
        path.write_text(content, encoding="utf-8")
        print(f"Updated {path.name} ({len(entries)} publications)")


if __name__ == "__main__":
    main()
