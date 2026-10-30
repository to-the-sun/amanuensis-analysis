#!/usr/bin/env python3

"""Calculator with safe expression evaluation, history, and memory.

Usage:
    python3 calculator.py                    # interactive mode
    python3 calculator.py "sqrt(2) * pi"     # one-shot evaluation

Interactive mode takes a whole expression such as ``2 + 3 * 4`` so the usual
precedence rules apply. Entering a bare number instead runs the original
step-by-step prompts (first number, operator, second number).
"""

from __future__ import annotations

import ast
import math
import sys

HISTORY_LIMIT = 50
PRECISION = 12
QUIT_WORDS = frozenset({"q", "quit", "exit"})

CONSTANTS = {"e": math.e, "pi": math.pi, "tau": math.tau}


# ---------------------------------------------------------------------------
# Arithmetic
# ---------------------------------------------------------------------------


def add(x, y):
    return x + y


def subtract(x, y):
    return x - y


def multiply(x, y):
    return x * y


def divide(x, y):
    if y == 0:
        raise ZeroDivisionError("Cannot divide by zero.")
    return x / y


def floor_divide(x, y):
    if y == 0:
        raise ZeroDivisionError("Cannot divide by zero.")
    return x // y


def modulo(x, y):
    if y == 0:
        raise ZeroDivisionError("Cannot take a remainder modulo zero.")
    return x % y


def power(x, y):
    try:
        return x**y
    except OverflowError:
        raise OverflowError("Result is too large to compute.") from None


def square_root(x):
    if x < 0:
        raise ValueError("Cannot take the square root of a negative number.")
    return math.sqrt(x)


def factorial(x):
    if isinstance(x, float):
        if not x.is_integer():
            raise ValueError("Factorial needs a whole number.")
        x = int(x)
    if x < 0:
        raise ValueError("Factorial needs a non-negative number.")
    if x > 1000:
        raise OverflowError("Factorial input is too large.")
    return math.factorial(x)


BINARY_OPERATORS = {
    ast.Add: add,
    ast.Sub: subtract,
    ast.Mult: multiply,
    ast.Div: divide,
    ast.FloorDiv: floor_divide,
    ast.Mod: modulo,
    ast.Pow: power,
}

UNARY_OPERATORS = {
    ast.UAdd: lambda value: value,
    ast.USub: lambda value: -value,
}

FUNCTIONS = {
    "abs": abs,
    "ceil": math.ceil,
    "cos": math.cos,
    "exp": math.exp,
    "factorial": factorial,
    "floor": math.floor,
    "hypot": math.hypot,
    "ln": math.log,
    "log": math.log10,
    "log2": math.log2,
    "round": round,
    "sin": math.sin,
    "sqrt": square_root,
    "tan": math.tan,
}


# ---------------------------------------------------------------------------
# Safe evaluation
# ---------------------------------------------------------------------------


def _evaluate_node(node, context):
    """Recursively evaluate a parsed expression, allowing only arithmetic."""
    if isinstance(node, ast.Expression):
        return _evaluate_node(node.body, context)

    if isinstance(node, ast.Constant):
        if isinstance(node.value, bool) or not isinstance(node.value, (int, float)):
            raise ValueError("Only numbers are allowed in expressions.")
        return node.value

    if isinstance(node, ast.Name):
        if node.id in context:
            return context[node.id]
        if node.id in CONSTANTS:
            return CONSTANTS[node.id]
        raise ValueError(f"Unknown name '{node.id}'.")

    if isinstance(node, ast.BinOp) and type(node.op) in BINARY_OPERATORS:
        left = _evaluate_node(node.left, context)
        right = _evaluate_node(node.right, context)
        return BINARY_OPERATORS[type(node.op)](left, right)

    if isinstance(node, ast.UnaryOp) and type(node.op) in UNARY_OPERATORS:
        return UNARY_OPERATORS[type(node.op)](_evaluate_node(node.operand, context))

    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id in FUNCTIONS
    ):
        if node.keywords:
            raise ValueError("Functions do not take keyword arguments.")
        arguments = [_evaluate_node(argument, context) for argument in node.args]
        return FUNCTIONS[node.func.id](*arguments)

    raise ValueError("That expression uses something this calculator cannot do.")


def evaluate(expression, context=None):
    """Evaluate an arithmetic expression without using ``eval``.

    ``^`` is accepted as a synonym for ``**``. Raises ``ValueError`` for
    anything that is not plain arithmetic over numbers.
    """
    cleaned = expression.strip().replace("^", "**")
    if not cleaned:
        raise ValueError("No expression given.")

    try:
        tree = ast.parse(cleaned, mode="eval")
    except SyntaxError:
        raise ValueError(f"Could not understand '{expression.strip()}'.") from None

    return _evaluate_node(tree, context or {})


def describe_error(exc):
    """Turn an exception into one readable line for the user."""
    message = str(exc)
    if "math domain error" in message:
        return "Result is outside the domain of that function."
    if isinstance(exc, ZeroDivisionError) and not message:
        return "Cannot divide by zero."
    if isinstance(exc, OverflowError):
        return message or "Result is too large to compute."
    return message or exc.__class__.__name__


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------


def format_number(value, precision=PRECISION):
    """Format a result without trailing ``.0`` noise or float dust."""
    if isinstance(value, complex):
        return f"({format_number(value.real, precision)}{value.imag:+.{precision}g}j)"
    if isinstance(value, int):
        return str(value)
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    if value.is_integer() and abs(value) < 1e16:
        return str(int(value))
    return f"{value:.{precision}g}"


# ---------------------------------------------------------------------------
# Calculator state
# ---------------------------------------------------------------------------


class Calculator:
    """Stateful calculator keeping a history list and a memory register."""

    def __init__(self, history_limit=HISTORY_LIMIT):
        self.history = []
        self.history_limit = history_limit
        self.memory = 0.0
        self.last_result = None

    @property
    def context(self):
        """Names available inside expressions."""
        values = {"ans": self.last_result, "mem": self.memory}
        return {name: value for name, value in values.items() if value is not None}

    def compute(self, expression):
        """Evaluate an expression and remember the result."""
        result = evaluate(expression, self.context)
        self.last_result = result
        self.history.append((expression.strip(), result))
        if len(self.history) > self.history_limit:
            self.history = self.history[-self.history_limit :]
        return result

    def print_history(self):
        if not self.history:
            print("No calculations yet.")
            return
        for index, (expression, result) in enumerate(self.history, start=1):
            print(f"  #{index:<3} {expression} = {format_number(result)}")

    def _operand(self, expression):
        if expression:
            return evaluate(expression, self.context)
        if self.last_result is None:
            raise ValueError("Nothing to use yet; give a value for this operation.")
        return self.last_result

    def memory_set(self, expression=""):
        self.memory = self._operand(expression)
        return self.memory

    def memory_add(self, expression=""):
        self.memory += self._operand(expression)
        return self.memory

    def memory_subtract(self, expression=""):
        self.memory -= self._operand(expression)
        return self.memory


HELP_TEXT = """\
Enter an expression using + - * / // % ** ^ and parentheses, e.g. 2 * (3 + 4).
Functions: abs, ceil, cos, exp, factorial, floor, hypot, ln, log, log2,
           round, sin, sqrt, tan.
Constants: e, pi, tau.        Variables: ans (last result), mem (memory).

A bare number starts the classic step-by-step prompts.

Commands:
  help              show this message
  history           list recent calculations
  clear             forget the history
  ms [expression]   store a value in memory (defaults to ans)
  m+ [expression]   add to memory (defaults to ans)
  m- [expression]   subtract from memory (defaults to ans)
  mr                show the memory register
  mc                clear the memory register
  q                 quit"""


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------


def run_command(line, calculator):
    """Run a command line. Returns True when the line was a command."""
    lowered = line.lower()

    if lowered == "help":
        print(HELP_TEXT)
        return True
    if lowered == "history":
        calculator.print_history()
        return True
    if lowered == "clear":
        calculator.history.clear()
        print("History cleared.")
        return True
    if lowered == "mr":
        print(f"Memory: {format_number(calculator.memory)}")
        return True
    if lowered == "mc":
        calculator.memory = 0.0
        print("Memory cleared.")
        return True

    memory_commands = (
        ("ms", calculator.memory_set),
        ("m+", calculator.memory_add),
        ("m-", calculator.memory_subtract),
    )
    for command, operation in memory_commands:
        if lowered == command or lowered.startswith(command + " "):
            try:
                value = operation(line[len(command) :].strip())
            except (ValueError, TypeError, ZeroDivisionError, OverflowError) as exc:
                print(f"Error: {describe_error(exc)}")
                return True
            print(f"Memory: {format_number(value)}")
            return True

    return False


STEP_OPERATORS = {
    "+": add,
    "-": subtract,
    "*": multiply,
    "/": divide,
    "^": power,
    "**": power,
    "%": modulo,
    "//": floor_divide,
}


def run_step_by_step(calculator, first_text):
    """Classic three-prompt flow, seeded with an already-entered number.

    Returns False if the user asked to quit.
    """
    operator = input("Enter operator (+, -, *, /, ^, %): ")
    if operator.strip().lower() in QUIT_WORDS:
        return False

    second_text = input("Enter second number: ")
    if second_text.strip().lower() in QUIT_WORDS:
        return False

    try:
        num1 = float(first_text)
        num2 = float(second_text)
    except ValueError:
        print("Invalid number. Please enter numeric values only.")
        return True

    operation = STEP_OPERATORS.get(operator.strip())
    if operation is None:
        print("Invalid operator. Please try again.")
        return True

    expression = f"{first_text.strip()} {operator.strip()} {second_text.strip()}"
    try:
        result = operation(num1, num2)
    except (ValueError, TypeError, ZeroDivisionError, OverflowError) as exc:
        print(describe_error(exc))
        return True

    calculator.last_result = result
    calculator.history.append((expression, result))
    if len(calculator.history) > calculator.history_limit:
        calculator.history = calculator.history[-calculator.history_limit :]
    print(f"Result: {format_number(result)}")
    return True


def is_bare_number(text):
    try:
        float(text)
    except ValueError:
        return False
    return True


def interactive(calculator):
    print("Simple Calculator")
    print("Enter an expression like 2 + 3 * 4, or 'help' for commands; q quits.")

    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nGoodbye!")
            return

        if not line:
            continue
        if line.lower() in QUIT_WORDS:
            print("Goodbye!")
            return
        if run_command(line, calculator):
            continue
        if is_bare_number(line):
            if not run_step_by_step(calculator, line):
                print("Goodbye!")
                return
            continue

        try:
            result = calculator.compute(line)
        except (ValueError, TypeError, ZeroDivisionError, OverflowError) as exc:
            print(f"Error: {describe_error(exc)}")
            continue
        print(f"Result: {format_number(result)}")


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    calculator = Calculator()

    if arguments:
        try:
            result = calculator.compute(" ".join(arguments))
        except (ValueError, TypeError, ZeroDivisionError, OverflowError) as exc:
            print(f"Error: {describe_error(exc)}", file=sys.stderr)
            return 1
        print(format_number(result))
        return 0

    interactive(calculator)
    return 0


if __name__ == "__main__":
    sys.exit(main())
