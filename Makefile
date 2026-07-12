.PHONY: build test vet tidy run

# mattn/go-sqlite3 needs cgo, so unlike rewsr-complete (which defaults
# CGO off), this repo defaults it on. Don't build or test with
# CGO_ENABLED=0 here; the sqlite3 driver will fail to compile.
CGO ?= 1
GOENV = CGO_ENABLED=$(CGO)

build:
	$(GOENV) go build -o rewsr-facts ./cmd/rewsr-facts

test:
	$(GOENV) go test ./...

vet:
	$(GOENV) go vet ./...

tidy:
	go mod tidy

run: build
	./rewsr-facts
