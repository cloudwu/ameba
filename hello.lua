a = ameba [[
	print("begin a",chan)

	for i=1,10 do
		r = recv()
		if r == nil then
			return
		end
		print("recv",r)
	end

]]

b = ameba [[
	print("begin b",chan)
	a = recv()
	print("recv in b",a);

	for i= 1, 20 do
		send(a, i * 2)
	end

]]

print(a,b)

send(b,a)

print ("hello world")