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
- `return` statement can be called with `$` to make it more easier. (not mandatory)
- Supported args are : `-i`(input), `-o`(output), `-d`(delimiter default to `%`).
- To print a value to the template need to use `return` or `$`.
- Simple example:
    * to print `0xaa + 10`:
    ```lua
    % function foo(num)
        return num + 10
    end %
    value = % $ foo(0xaa) %
    ```
    * run: tmg -i test.txt -o value.txt
    * output:
    ```ini

    value = 180
    ```
