package main

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/dunglas/frankenphp"
)

func main() {
	// Initialize FrankenPHP with TrueAsync mode
	err := frankenphp.Init(
		frankenphp.WithNumThreads(0),                            // No regular threads
		frankenphp.WithAsyncMode("/home/edmond/frankenphp/test_async.php", 1), // 1 async thread
	)
	if err != nil {
		log.Fatal(err)
	}
	defer frankenphp.Shutdown()

	fmt.Println("[TEST] FrankenPHP initialized with TrueAsync")

	// Start HTTP server
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		err := frankenphp.ServeHTTP(w, r)
		if err != nil {
			log.Printf("[ERROR] ServeHTTP failed: %v", err)
		}
	})

	server := &http.Server{
		Addr:    ":8080",
		Handler: handler,
	}

	go func() {
		fmt.Println("[TEST] Starting HTTP server on :8080")
		if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}()

	// Wait for server to start
	time.Sleep(1 * time.Second)

	// Test request
	fmt.Println("[TEST] Sending test request...")
	resp, err := http.Get("http://localhost:8080/")
	if err != nil {
		log.Fatalf("[ERROR] Request failed: %v", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		log.Fatalf("[ERROR] Failed to read response: %v", err)
	}

	fmt.Printf("[TEST] Response status: %d\n", resp.StatusCode)
	fmt.Printf("[TEST] Response body: %s\n", string(body))

	// Shutdown
	time.Sleep(500 * time.Millisecond)
	server.Close()
	fmt.Println("[TEST] Test completed!")
}
