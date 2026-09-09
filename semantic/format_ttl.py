"""Format compact Turtle declarations without expanding prefixes or ``a``."""

from pathlib import Path
import re


ROOT = Path(__file__).parent / "ontology"
LIST_PREDICATES = {
    "sf:hasColumn",
    "sf:hasParameter",
    "sf:hasProperty",
}


def split_predicates(text):
    parts = []
    start = 0
    quoted = False
    escaped = False
    depth = 0
    for index, char in enumerate(text):
        if char == '"' and not escaped:
            quoted = not quoted
        elif not quoted and char in "([":
            depth += 1
        elif not quoted and char in ")]":
            depth -= 1
        if char == ";" and not quoted and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
        escaped = char == "\\" and not escaped
    parts.append(text[start:].strip())
    return [part for part in parts if part]


def split_objects(text):
    parts = []
    start = 0
    quoted = False
    escaped = False
    depth = 0
    for index, char in enumerate(text):
        if char == '"' and not escaped:
            quoted = not quoted
        elif not quoted and char in "([":
            depth += 1
        elif not quoted and char in ")]":
            depth -= 1
        elif char == "," and not quoted and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
        escaped = char == "\\" and not escaped
    parts.append(text[start:].strip())
    return [part for part in parts if part]


def format_predicate(predicate):
    name, objects = predicate.strip().split(None, 1)
    terminator = "." if objects.endswith(".") else ";"
    if objects.endswith((";", ".")):
        objects = objects[:-1].rstrip()
    values = split_objects(objects)
    if name not in LIST_PREDICATES or len(values) < 2:
        return [f"    {name} {objects} {terminator}"]
    return [f"    {name}"] + [
        f"        {value}{',' if index < len(values) - 1 else f' {terminator}'}"
        for index, value in enumerate(values)
    ]


def is_subject_start(stripped):
    """Return whether a line starts a subject declaration.

    Subject lines can be indented in hand-authored Turtle.  Predicate and
    object continuation lines must remain indented, so identify subjects by
    their prefixed name followed by a declaration predicate.
    """
    return bool(
        re.match(
            r"^(?:sfcore|sfms|sfram|sfsensors):[^\s]+\s+(?:a|sf:|skos:)",
            stripped,
        )
    )


def is_subject_name(stripped):
    return bool(re.match(r"^(?:sfcore|sfms|sfram|sfsensors):[^\s]+$", stripped))


def format_file(path):
    output = []
    lines = path.read_text().splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        stripped = line.strip()
        predicate_name = stripped.split(None, 1)[0] if stripped else ""
        if is_subject_name(stripped):
            declaration = stripped
            index += 1
            while index < len(lines):
                declaration += " " + lines[index].strip()
                if declaration.endswith("."):
                    break
                index += 1
            subject, predicates = declaration.split(None, 1)
            parts = split_predicates(predicates)
            output.append(subject)
            for part_index, predicate in enumerate(parts):
                if part_index < len(parts) - 1 and predicate.endswith("."):
                    predicate = predicate[:-1].rstrip() + " ;"
                output.extend(format_predicate(predicate))
            index += 1
            continue
        if predicate_name in LIST_PREDICATES:
            predicate = stripped
            index += 1
            while not predicate.endswith((";", ".")) and index < len(lines):
                predicate += " " + lines[index].strip()
                index += 1
            output.extend(format_predicate(predicate))
            continue
        if (
            not stripped
            or stripped.startswith("@prefix")
            or (line[:1].isspace() and not is_subject_start(stripped))
            or ";" not in stripped
        ):
            output.append(line)
            index += 1
            continue
        subject, predicates = stripped.split(None, 1)
        parts = split_predicates(predicates)
        output.append(subject)
        for part_index, predicate in enumerate(parts):
            if part_index < len(parts) - 1 and predicate.endswith("."):
                predicate = predicate[:-1].rstrip() + " ;"
            output.extend(format_predicate(predicate))
        index += 1
    spaced = []
    for line in output:
        if line.startswith("  ") and not line.startswith("    "):
            line = "  " + line
        is_definition = bool(line) and not line[0].isspace() and not line.startswith("@prefix")
        if is_definition and spaced and spaced[-1] != "":
            spaced.append("")
        spaced.append(line)
    path.write_text("\n".join(spaced) + "\n")


def main():
    for path in ROOT.rglob("*.ttl"):
        format_file(path)


if __name__ == "__main__":
    main()
