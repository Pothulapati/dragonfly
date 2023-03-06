/*
Copyright 2023.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package controller

import (
	"context"
	"fmt"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	dfdb "dragonflydb.io/dragonfly/api/v1alpha1"
	"dragonflydb.io/dragonfly/internal/resources"
)

// DragonflyDbReconciler reconciles a DragonflyDb object
type DragonflyDbReconciler struct {
	client.Client
	Scheme *runtime.Scheme
}

//+kubebuilder:rbac:groups=dragonflydb.io,resources=dragonflydbs,verbs=get;list;watch;create;update;patch;delete
//+kubebuilder:rbac:groups=dragonflydb.io,resources=dragonflydbs/status,verbs=get;update;patch
//+kubebuilder:rbac:groups=dragonflydb.io,resources=dragonflydbs/finalizers,verbs=update

// Reconcile is part of the main kubernetes reconciliation loop which aims to
// move the current state of the cluster closer to the desired state.
// TODO(user): Modify the Reconcile function to compare the state specified by
// the DragonflyDb object against the actual cluster state, and then
// perform operations to make the cluster state reflect the state specified by
// the user.
//
// For more details, check Reconcile and its Result here:
// - https://pkg.go.dev/sigs.k8s.io/controller-runtime@v0.14.1/pkg/reconcile
func (r *DragonflyDbReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	log := log.FromContext(ctx)

	var db dfdb.DragonflyDb
	if err := r.Get(ctx, req.NamespacedName, &db); err != nil {
		log.Info(fmt.Sprintf("could not get the Database object: %s", req.NamespacedName))
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	log.Info("Reconciling Database object")
	// Ignore if resource is already created
	// TODO: Handle updates to the Database object
	if !db.Status.Created {
		log.Info("Creating resources")
		resources, err := resources.GetDatabaseResources(ctx, &db)
		if err != nil {
			log.Error(err, "could not get resources")
			return ctrl.Result{}, err
		}

		// create all resources
		for _, resource := range resources {
			if err := r.Create(ctx, resource); err != nil {
				log.Error(err, "could not create resource")
				return ctrl.Result{}, err
			}
		}

		log.Info("Waiting for the statefulset to be ready")
		/*
			if err := waitForStatefulSetReady(ctx, r.Client, db.Name, db.Namespace, 1*time.Minute); err != nil {
				log.Error(err, "could not wait for statefulset to be ready")
				return ctrl.Result{}, err
			}

			if err := findHealthyAndMarkActive(ctx, r.Client, &db); err != nil {
				log.Error(err, "could not find healthy and mark active")
				return ctrl.Result{}, err
			}
		*/

		// Update Status
		db.Status.Created = true
		log.Info("Created resources for object")
		if err := r.Status().Update(ctx, &db); err != nil {
			log.Error(err, "could not update the Database object")
			return ctrl.Result{}, err
		}
	}

	return ctrl.Result{}, nil
}

// SetupWithManager sets up the controller with the Manager.
func (r *DragonflyDbReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&dfdb.DragonflyDb{}).
		Complete(r)
}
