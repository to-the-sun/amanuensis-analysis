#!/usr/bin/env python3

"""Basic calculator script."""


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


def main():
    print("Simple Calculator")
    print("Use +, -, *, /, or q to quit.")

    while True:
        try:
            first_number = input("Enter first number: ")
            if first_number.strip().lower() == 'q':
                print("Goodbye!")
                break

            operator = input("Enter operator (+, -, *, /): ")
            if operator.strip().lower() == 'q':
                print("Goodbye!")
                break

            second_number = input("Enter second number: ")
            if second_number.strip().lower() == 'q':
                print("Goodbye!")
                break

            num1 = float(first_number)
            num2 = float(second_number)

            if operator == '+':
                result = add(num1, num2)
            elif operator == '-':
                result = subtract(num1, num2)
            elif operator == '*':
                result = multiply(num1, num2)
            elif operator == '/':
                result = divide(num1, num2)
            else:
                print("Invalid operator. Please try again.")
                continue

            print(f"Result: {result}")

        except ValueError:
            print("Invalid number. Please enter numeric values only.")
        except ZeroDivisionError as exc:
            print(exc)


if __name__ == "__main__":
    main()
