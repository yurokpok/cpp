def calculate_average(numbers):
    total = 0
    for i in range(len(numbers)):
        total += numbers[i]
    return total / len(numbers) 

def process_data(data):
    x = None
    if data:
        x = data[0]
    return x.upper()  

result1 = calculate_average([])  
result2 = process_data([])     

print("Results:", result1, result2)