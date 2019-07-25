#!/bin/bash

QPs=(32 37)
Dir=blowing40

for qp in "${QPs[@]}";
do
	cd ~/git-repos/HM16.9/DL/$Dir/$qp
    List=(emb* lins*-weight.csv outp-weight.csv lins*-bias.csv outp-bias.csv bn-weight.csv bns*-weight.csv bns*-bias.csv mapper*)
	
	for file in *;
	do
        truncate -s-2 $file
        sed -i -e 's/^/\t\t\t/' $file
        echo ";" >> "$file"
	done
	
	for i in "${!List[@]}";
	do
		mv "${List[$i]}" "$((i+1)).${List[$i]}"
	done
done











