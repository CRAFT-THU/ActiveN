import fileinput

tot = 0
for line in fileinput.input():
  tot += float(line.rstrip())
print(tot)
