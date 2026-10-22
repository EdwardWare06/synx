# 15. Project: a training log

This chapter builds one complete program. The program reads a training log.
Each line holds one set: the name of a lift, the weight in pounds, and the
number of reps:

```text
squat 225 5
```

For each lift, the program finds the number of sets, the volume, and the best
set. The volume is the sum of pounds times reps over all sets. The best set
has the most weight. The program prints the lifts in order of volume, from
the largest. It reports each bad line and continues.

The program uses most of the earlier chapters: structs, `Result` and `?`,
`String`, `Vec`, `HashMap`, sorting, traits, and files.

## The input

The program writes its own input file, `training.txt`, before it reads it.
This keeps the example in one file. The log has a blank line and two bad
lines:

```text
squat 225 5
squat 245 3
squat 225 5
bench 185 5
bench 205 3
bench 185 five

deadlift 315 5
deadlift 365 2
row 135
overhead 115 8
```

Line 6 has a word where a number must be. Line 10 has only two fields.

## Parsing one line

An `Entry` holds one set. `parse_line` turns one line of text into an
`Entry`, or into an error message. The message is a `String`, so it can
contain the line number and the bad text:

```synx
struct Entry {
    String lift;
    int pounds;
    int reps;
}

Result<int, String> number(&const String field, &const str name, int line) {
    int value = field.as_str().parse_int() ?? 0;
    if (value <= 0) {
        return Err("line {line}: {name} '{field}' is not a positive number");
    }
    return Ok(value);
}

Result<Entry, String> parse_line(&const String text, int line) {
    var fields = Vec<String>.new();
    var parts = text.split(" ");
    for (part in &const parts) {
        if (!part.is_empty()) {
            fields.push(String.from(part.as_str()));
        }
    }
    if (fields.len() != 3) {
        return Err("line {line}: expected 'lift pounds reps', found '{text}'");
    }
    int pounds = number(fields[1], "pounds", line)?;
    int reps = number(fields[2], "reps", line)?;
    return Ok(Entry { lift: String.from(fields[0].as_str()), pounds: pounds, reps: reps });
}

void check(&const str text, int line) {
    match parse_line(String.from(text), line) {
        Ok(entry) => print("{entry.lift}: {entry.pounds} lb x {entry.reps}"),
        Err(message) => print("error: {message}"),
    }
}

main() {
    check("squat 225 5", 1);
    check("bench  185   5", 2);
    check("bench 185 five", 3);
    check("deadlift -315 5", 4);
    check("row 135", 5);
}
```

```text
squat: 225 lb x 5
bench: 185 lb x 5
error: line 3: reps 'five' is not a positive number
error: line 4: pounds '-315' is not a positive number
error: line 5: expected 'lift pounds reps', found 'row 135'
```

`split(" ")` gives an empty field between two spaces. The loop drops the
empty fields, so extra spaces between the values are permitted.

`parse_int` returns an `int?`. The `??` operator gives 0 when the text is not
a number, and the check `value <= 0` then rejects it. The `?` after each call
to `number` returns the error from `parse_line` at once.

The fields stay in the `Vec`. `String.from(fields[0].as_str())` copies the
name into the new `Entry`.

## Grouping by lift

A `Summary` holds the totals for one lift:

```synx
struct Summary {
    String lift;
    int sets;
    long volume;
    int best_pounds;
    int best_reps;
}
```

The summaries are in a `Vec`, because the program must sort them later. A
`HashMap` maps the name of each lift to the position of its summary in the
`Vec`. `HashMap` has no method that visits all of its entries, so the `Vec`
is also the only way to list the lifts:

```synx
void add(&Vec<Summary> summaries, &HashMap<String, usize> index, &const Entry entry) {
    if (!index.contains_key(entry.lift)) {
        index.insert(String.from(entry.lift.as_str()), summaries.len());
        summaries.push(Summary { lift: String.from(entry.lift.as_str()), sets: 0, volume: 0, best_pounds: 0, best_reps: 0 });
    }
    usize at = *index.get(entry.lift).unwrap();
    var summary = &summaries[at];
    summary.sets += 1;
    summary.volume += (entry.pounds * entry.reps) as long;
    if (entry.pounds > summary.best_pounds ||
        (entry.pounds == summary.best_pounds && entry.reps > summary.best_reps)) {
        summary.best_pounds = entry.pounds;
        summary.best_reps = entry.reps;
    }
}
```

A new lift first gets an empty summary. Then every set takes the same path.
`index.get` returns a reference to the stored position, and `*` copies the
`usize` out of it. `&summaries[at]` is a writable reference to one element,
so the changes go into the `Vec`. When two sets have the same weight, the set
with more reps is the best set.

## Sorting the result

`std.collections.sort` sorts a `Vec` of any type that implements `Ord`. The
`Ord` trait has one method, `lt`, which returns true when `self` comes before
`other`. A larger volume comes first. When two volumes are equal, the names
decide the order:

```synx
impl Ord for Summary {
    bool lt(&const self, &const Self other) {
        if (self.volume != other.volume) {
            return self.volume > other.volume;
        }
        return self.lift < other.lift;
    }
}
```

`sort` is not stable. Equal elements can change places. The comparison of
the names makes the order complete, so the report is always the same.

## The report

The report prints one row for each lift. `pad` adds spaces to the right of a
text until it has the given width:

```synx
String pad(&const str text, usize width) {
    var result = String.from(text);
    while (result.len() < width) {
        result.push(' ');
    }
    return result;
}
```

`len` counts bytes. All the text in this report is ASCII, so the byte count
is also the character count.

## The complete program

`load` writes the sample file and reads it back. Each step that can fail
uses `?`, so the first I/O error goes to `main`. `main` splits the text into
lines, skips blank lines, and parses the rest. A bad line prints a message,
and the program continues with the next line. The exit status is 1 when the
program skipped a line:

```synx
import std.fs;
import std.fs.{File, IoError};
import std.text;
import std.collections.{HashMap, sort};

struct Entry {
    String lift;
    int pounds;
    int reps;
}

struct Summary {
    String lift;
    int sets;
    long volume;
    int best_pounds;
    int best_reps;
}

impl Ord for Summary {
    bool lt(&const self, &const Self other) {
        if (self.volume != other.volume) {
            return self.volume > other.volume;
        }
        return self.lift < other.lift;
    }
}

Result<String, IoError> load() {
    var file = File.create("training.txt")?;
    file.write_text("squat 225 5\nsquat 245 3\nsquat 225 5\n")?;
    file.write_text("bench 185 5\nbench 205 3\nbench 185 five\n\n")?;
    file.write_text("deadlift 315 5\ndeadlift 365 2\nrow 135\n")?;
    file.write_text("overhead 115 8\n")?;
    file.close()?;
    return fs.read_text("training.txt");
}

Result<int, String> number(&const String field, &const str name, int line) {
    int value = field.as_str().parse_int() ?? 0;
    if (value <= 0) {
        return Err("line {line}: {name} '{field}' is not a positive number");
    }
    return Ok(value);
}

Result<Entry, String> parse_line(&const String text, int line) {
    var fields = Vec<String>.new();
    var parts = text.split(" ");
    for (part in &const parts) {
        if (!part.is_empty()) {
            fields.push(String.from(part.as_str()));
        }
    }
    if (fields.len() != 3) {
        return Err("line {line}: expected 'lift pounds reps', found '{text}'");
    }
    int pounds = number(fields[1], "pounds", line)?;
    int reps = number(fields[2], "reps", line)?;
    return Ok(Entry { lift: String.from(fields[0].as_str()), pounds: pounds, reps: reps });
}

void add(&Vec<Summary> summaries, &HashMap<String, usize> index, &const Entry entry) {
    if (!index.contains_key(entry.lift)) {
        index.insert(String.from(entry.lift.as_str()), summaries.len());
        summaries.push(Summary { lift: String.from(entry.lift.as_str()), sets: 0, volume: 0, best_pounds: 0, best_reps: 0 });
    }
    usize at = *index.get(entry.lift).unwrap();
    var summary = &summaries[at];
    summary.sets += 1;
    summary.volume += (entry.pounds * entry.reps) as long;
    if (entry.pounds > summary.best_pounds ||
        (entry.pounds == summary.best_pounds && entry.reps > summary.best_reps)) {
        summary.best_pounds = entry.pounds;
        summary.best_reps = entry.reps;
    }
}

String pad(&const str text, usize width) {
    var result = String.from(text);
    while (result.len() < width) {
        result.push(' ');
    }
    return result;
}

int main() {
    String contents;
    match load() {
        Ok(text) => { contents = text; }
        Err(error) => {
            print("error: cannot use training.txt (OS code {error.os_code})");
            return 1;
        }
    }

    var summaries = Vec<Summary>.new();
    var index = HashMap<String, usize>.new();
    int skipped = 0;
    var lines = text.lines(contents);
    for (i in 0..lines.len()) {
        if (lines[i].trim().is_empty()) {
            continue;
        }
        match parse_line(lines[i], i as int + 1) {
            Ok(entry) => add(&summaries, &index, entry),
            Err(message) => {
                print("skipped {message}");
                skipped += 1;
            }
        }
    }

    sort(&summaries);
    print("");
    print("{pad("lift", 10)}{pad("sets", 6)}{pad("volume", 8)}best set");
    long total = 0;
    for (s in &const summaries) {
        print("{pad(s.lift, 10)}{pad("{s.sets}", 6)}{pad("{s.volume}", 8)}{s.best_pounds} x {s.best_reps}");
        total += s.volume;
    }
    print("");
    print("total volume: {total} lb");
    return if (skipped > 0) { 1 } else { 0 };
}
```

```text
skipped line 6: reps 'five' is not a positive number
skipped line 10: expected 'lift pounds reps', found 'row 135'

lift      sets  volume  best set
squat     3     2985    245 x 3
deadlift  2     2305    365 x 2
bench     2     1540    205 x 3
overhead  1     920     115 x 8

total volume: 7750 lb
```

The squat volume is 225 x 5 + 245 x 3 + 225 x 5 = 2985 pounds. The exit
status of this run is 1, because two lines were skipped.

`text.lines` returns a `Vec<String>`. The loop uses an index, because the
line number is the index plus 1. `lines[i]` passes a shared reference to
`parse_line`, and the `Vec` keeps its strings.

## Changes to try

- Read the log from standard input with `std.io.read_line` in place of the
  file. `read_line` returns `Ok(None)` at the end of the input.
- Take the file name from `std.env.args`, as chapter 13 shows.
- Add a date line, such as `# 2026-10-22`, and group the sets by day.

## Next

The tutorial ends here. The [language reference](../../reference/index.md)
gives the exact rules for each construct. The
[library reference](../../library/index.md) describes each standard module.
