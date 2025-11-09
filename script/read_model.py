import sys
import tensorflow as tf

if len(sys.argv) < 2:
    print("Usage: python inspect_tflite.py <model.tflite>")
    sys.exit(1)

model_path = sys.argv[1]
interpreter = tf.lite.Interpreter(model_path=model_path)
interpreter.allocate_tensors()

input_details = interpreter.get_input_details()
output_details = interpreter.get_output_details()

print("=== MODEL INSPECTION ===")
print(f"Model: {model_path}\n")

print("INPUT DETAILS:")
for d in input_details:
    print(f"  Name: {d['name']}")
    print(f"  Shape: {d['shape']}")
    print(f"  DType: {d['dtype']}\n")

print("OUTPUT DETAILS:")
for d in output_details:
    print(f"  Name: {d['name']}")
    print(f"  Shape: {d['shape']}")
    print(f"  DType: {d['dtype']}\n")