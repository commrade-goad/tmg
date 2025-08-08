# TMG
a simple template engine that integrate with `lua` so you can script your template.

## Build
1. __Dependency:__
    * `lua`

2. __Build:__

```sh
make all -j8
```

## Usage
- Supported args are : `-i`(input), `-o`(output), `-d`(delimiter default to `%`).
- To print a value to the template need to use `$` (mean inline the func and just print that).
- See `example` for more info.
