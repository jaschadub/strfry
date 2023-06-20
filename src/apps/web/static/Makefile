CSS := reset.css oddbean.css
JS  := turbo.js oddbean.js

.PHONY: all css js

all: css js

css: $(CSS)
	mkdir -p build/
	cat $(CSS) | sassc -s -t compressed > build/oddbean.css
	gzip -k9f build/oddbean.css

js: $(JS)
	mkdir -p build/
	cat $(JS) | esbuild --minify > build/oddbean.js
	gzip -k9f build/oddbean.js
