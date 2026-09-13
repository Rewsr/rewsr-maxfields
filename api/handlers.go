package api

import (
	"encoding/json"
	"log"
	"net/http"
	"time"

	"github.com/Rewsr/rewsr-maxfields/store"
)

// Server serves the HTTP API from a FactsStore. It never holds a
// facts.Collector: handlers only ever read cached facts, they don't
// trigger collection.
type Server struct {
	Store store.FactsStore
}

// NewServer returns a Server reading from s.
func NewServer(s store.FactsStore) *Server {
	return &Server{Store: s}
}

// Routes returns the HTTP handler for the facts API.
func (s *Server) Routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.handleHealthz)
	mux.HandleFunc("GET /servers/{id}", s.handleGetServer)
	mux.HandleFunc("GET /servers/{id}/hardware", s.handleGetServerHardware)
	return mux
}

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"healthy":   true,
		"service":   "rewsr-maxfields",
		"timestamp": time.Now().UTC().Format(time.RFC3339),
	})
}

// handleGetServer serves GET /servers/{id}: the curated public view.
func (s *Server) handleGetServer(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if id == "" {
		writeError(w, http.StatusBadRequest, "server id required")
		return
	}

	f, err := s.Store.GetLatestFacts(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusNotFound, "no facts on record for server "+id)
		return
	}

	writeJSON(w, http.StatusOK, PublicView(f))
}

// handleGetServerHardware serves GET /servers/{id}/hardware: the curated
// advanced-user hardware view.
func (s *Server) handleGetServerHardware(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if id == "" {
		writeError(w, http.StatusBadRequest, "server id required")
		return
	}

	f, err := s.Store.GetLatestFacts(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusNotFound, "no facts on record for server "+id)
		return
	}

	writeJSON(w, http.StatusOK, HardwareView(f))
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("rewsr-maxfields: encoding response: %v", err)
	}
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}
